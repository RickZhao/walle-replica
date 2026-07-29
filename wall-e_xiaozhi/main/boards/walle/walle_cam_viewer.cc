/**
 * WALL-E CAMERA VIEWER
 *
 * @file    walle_cam_viewer.cc
 * @brief   Implementation, see walle_cam_viewer.h
 */

#include "walle_cam_viewer.h"
#include "config.h"

#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "lvgl_image.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_jpeg_common.h>
#include <esp_jpeg_dec.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#define TAG "WalleCamViewer"

// Caps for PSRAM buffers (ESP32-S3 N16R8 has 8MB PSRAM).
static constexpr size_t kFilesJsonCap    = 16 * 1024;   // /files listing
static constexpr size_t kJpegCap         = 384 * 1024;  // one JPEG photo / video frame
static constexpr size_t kIdxTailCap      = 256 * 1024;  // idx1 must fit in this tail window (~3h @15fps)
static constexpr int    kMaxDecodedDim   = 480;         // scale JPEG down until both dims <= this
static constexpr size_t kAviHeadSize     = 256;         // RIFF header probe
// AVI layout written by wall-e_esp32_cam/avi_writer.h: movi data starts at a
// fixed offset; idx1 entries are 16 bytes ("00dc", flags, offset, size) and
// offsets are relative to the movi data start (frame = movi_start + off + 8).
static constexpr size_t kAviMoviFallback = 224;


WalleCamViewer& WalleCamViewer::GetInstance() {
    static WalleCamViewer instance;
    return instance;
}


// -------------------------------------------------------------------
// HTTP helpers
// -------------------------------------------------------------------

struct HttpSink {
    uint8_t* data;
    size_t cap;
    size_t len = 0;
    bool overflow = false;
};

static esp_err_t HttpSinkEvent(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
        auto* sink = static_cast<HttpSink*>(evt->user_data);
        if (sink->len + (size_t)evt->data_len > sink->cap) {
            sink->overflow = true;
            return ESP_FAIL;
        }
        memcpy(sink->data + sink->len, evt->data, evt->data_len);
        sink->len += evt->data_len;
    }
    return ESP_OK;
}

/// GET CAM_MODULE_URL + path into a preallocated sink. range_start < 0 fetches
/// the whole resource, otherwise sends "Range: bytes=start-end" (end <
/// start = open-ended). Accepts HTTP 200/206. Returns byte count, 0 on error.
static size_t HttpGet(const std::string& path, long range_start, long range_end,
                      uint8_t* buf, size_t cap, size_t* received, int timeout_ms = 5000) {
    std::string url = std::string(CAM_MODULE_URL) + path;
    HttpSink sink{buf, cap, 0, false};

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = timeout_ms;
    cfg.event_handler = HttpSinkEvent;
    cfg.user_data = &sink;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (range_start >= 0) {
        char range_hdr[48];
        if (range_end >= range_start) {
            snprintf(range_hdr, sizeof(range_hdr), "bytes=%ld-%ld", range_start, range_end);
        } else {
            snprintf(range_hdr, sizeof(range_hdr), "bytes=%ld-", range_start);
        }
        esp_http_client_set_header(client, "Range", range_hdr);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || sink.overflow) {
        ESP_LOGW(TAG, "GET %s failed: %s%s", url.c_str(), esp_err_to_name(err),
                 sink.overflow ? " (buffer overflow)" : "");
        return 0;
    }
    if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url.c_str(), status);
        return 0;
    }
    *received = sink.len;
    return sink.len;
}

/// Allocate a PSRAM buffer. Caller frees with heap_caps_free().
static uint8_t* PsramAlloc(size_t size) {
    return static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}


// -------------------------------------------------------------------
// /files listing scan (no JSON parser in this component; the cam's
// format is fixed: {"path":"<p>","size":<n>} objects)
// -------------------------------------------------------------------

/// Find the lexicographically greatest path starting with `dir_prefix`
/// (zero-padded numbering => latest file). Returns false when none.
static bool FindLatestFile(const char* json, const char* dir_prefix,
                           char* path_out, size_t path_out_size, size_t* size_out) {
    std::string needle = std::string("\"path\":\"") + dir_prefix;
    std::string best;
    size_t best_size = 0;
    size_t pos = 0;
    while ((pos = std::string(json).find(needle, pos)) != std::string::npos) {
        const char* pstart = json + pos + needle.size();
        const char* pend = strchr(pstart, '"');
        if (!pend) break;
        std::string path(pstart, pend - pstart);

        // size follows the path inside the same object
        size_t fsize = 0;
        const char* skey = strstr(pend, "\"size\":");
        if (skey) fsize = (size_t)atol(skey + 7);

        if (best.empty() || path > best) {
            best = path;
            best_size = fsize;
        }
        pos = pend - json;
    }
    if (best.empty()) return false;
    strlcpy(path_out, best.c_str(), path_out_size);
    *size_out = best_size;
    return true;
}

/// Fetch /files and locate the newest file under dir_prefix.
static std::string FindLatestOnCam(const char* dir_prefix,
                                   char* path_out, size_t path_out_size, size_t* size_out) {
    uint8_t* buf = PsramAlloc(kFilesJsonCap + 1);
    if (!buf) return "out of memory";
    size_t received = 0;
    size_t got = HttpGet("/files", -1, -1, buf, kFilesJsonCap, &received);
    std::string err;
    if (got == 0) {
        err = "camera module unreachable";
    } else {
        buf[received] = '\0';
        if (!FindLatestFile(reinterpret_cast<const char*>(buf), dir_prefix,
                            path_out, path_out_size, size_out)) {
            err = std::string("no files under ") + dir_prefix;
        }
    }
    heap_caps_free(buf);
    return err;
}


// -------------------------------------------------------------------
// JPEG decode (software esp_new_jpeg, with optional 1/2^n downscale)
// -------------------------------------------------------------------

static bool DecodeJpegOnce(const uint8_t* src, size_t src_len, int sw, int sh,
                           uint8_t** out_buf, int* out_len) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (sw > 0 && sh > 0) {
        cfg.scale.width = sw;
        cfg.scale.height = sh;
    }
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;

    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(src);
    io.inbuf_len = (int)src_len;
    jpeg_dec_header_info_t info = {};

    bool ok = false;
    int outbuf_len = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK &&
        jpeg_dec_get_outbuf_len(dec, &outbuf_len) == JPEG_ERR_OK && outbuf_len > 0) {
        uint8_t* buf = static_cast<uint8_t*>(jpeg_calloc_align(outbuf_len, 16));
        if (buf) {
            io.outbuf = buf;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                *out_buf = buf;
                *out_len = outbuf_len;
                ok = true;
            } else {
                jpeg_free_align(buf);
            }
        }
    }
    jpeg_dec_close(dec);
    return ok;
}

/// Decode JPEG to RGB565 (LE), halving dimensions while either exceeds
/// kMaxDecodedDim (decoder scale requires multiples of 8, max ratio 1/8).
/// On success *out_buf is a jpeg_calloc_align buffer (freed later by
/// LvglAllocatedImage via heap_caps_free, same as esp_video.cc).
static bool DecodeJpeg(const uint8_t* src, size_t src_len,
                       uint8_t** out_buf, int* out_w, int* out_h, int* out_len) {
    // Stage 1: header only, to learn the source dimensions.
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(src);
    io.inbuf_len = (int)src_len;
    jpeg_dec_header_info_t info = {};
    jpeg_error_t err = jpeg_dec_parse_header(dec, &io, &info);
    jpeg_dec_close(dec);
    if (err != JPEG_ERR_OK) return false;

    int sw = info.width, sh = info.height;
    while (sw > kMaxDecodedDim || sh > kMaxDecodedDim) {
        int nw = (sw / 2) & ~7, nh = (sh / 2) & ~7;
        if (nw < 8 || nh < 8) break;
        sw = nw;
        sh = nh;
    }
    bool scaled = (sw != info.width || sh != info.height);

    if (DecodeJpegOnce(src, src_len, scaled ? sw : 0, scaled ? sh : 0, out_buf, out_len)) {
        *out_w = scaled ? sw : info.width;
        *out_h = scaled ? sh : info.height;
        return true;
    }
    if (scaled) {
        ESP_LOGW(TAG, "Scaled decode failed, retrying full-res");
        if (DecodeJpegOnce(src, src_len, 0, 0, out_buf, out_len)) {
            *out_w = info.width;
            *out_h = info.height;
            return true;
        }
    }
    return false;
}

/// Wrap a decoded RGB565 buffer and show it on the eye display.
static void DisplayFrame(uint8_t* buf, int len, int w, int h) {
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display == nullptr) {
        jpeg_free_align(buf);
        return;
    }
    auto image = std::make_unique<LvglAllocatedImage>(buf, len, w, h, w * 2, LV_COLOR_FORMAT_RGB565);
    display->SetPreviewImage(std::move(image));
}

static void RestoreEyes() {
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        display->SetPreviewImage(nullptr);
    }
}


// -------------------------------------------------------------------
// Worker task
// -------------------------------------------------------------------

static uint32_t ReadU32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void WalleCamViewer::WorkerEntry(void* arg) {
    auto* self = static_cast<WalleCamViewer*>(arg);
    self->WorkerMain();
    self->busy_.store(false);
    self->worker_ = nullptr;
    vTaskDelete(nullptr);
}

void WalleCamViewer::WorkerMain() {
    if (op_ == Op::kPhoto) {
        RunPhoto();
    } else {
        RunVideo();
    }
}

void WalleCamViewer::RunPhoto() {
    size_t cap = (op_file_size_ > 0 && op_file_size_ <= kJpegCap) ? op_file_size_ : kJpegCap;
    uint8_t* jpg = PsramAlloc(cap);
    if (!jpg) {
        ESP_LOGE(TAG, "No memory for photo download");
        return;
    }
    std::string url_path = std::string("/file?path=") + op_path_;
    size_t received = 0;
    size_t got = HttpGet(url_path, -1, -1, jpg, cap, &received, 15000);
    if (got == 0) {
        heap_caps_free(jpg);
        return;
    }

    uint8_t* rgb = nullptr;
    int w = 0, h = 0, rgb_len = 0;
    if (!DecodeJpeg(jpg, received, &rgb, &w, &h, &rgb_len)) {
        ESP_LOGE(TAG, "Failed to decode photo %s", op_path_);
        heap_caps_free(jpg);
        return;
    }
    heap_caps_free(jpg);
    ESP_LOGI(TAG, "Showing photo %s (%dx%d)", op_path_, w, h);
    DisplayFrame(rgb, rgb_len, w, h);
    // The display's own 5s preview timer switches back to the eyes.
}

void WalleCamViewer::RunVideo() {
    if (op_file_size_ < kAviHeadSize + 32) {
        ESP_LOGE(TAG, "Video %s too small / unknown size (%u)", op_path_, (unsigned)op_file_size_);
        return;
    }
    std::string url_path = std::string("/file?path=") + op_path_;

    // ---- probe the RIFF header ----
    uint8_t head[kAviHeadSize];
    size_t received = 0;
    if (HttpGet(url_path, 0, kAviHeadSize - 1, head, sizeof(head), &received) == 0 ||
        received < kAviHeadSize) {
        ESP_LOGE(TAG, "Failed to fetch AVI header");
        return;
    }
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "AVI ", 4) != 0) {
        ESP_LOGE(TAG, "%s is not an AVI file", op_path_);
        return;
    }
    // avih.dwMicroSecPerFrame sits at a fixed offset in our writer's layout
    uint32_t frame_us = ReadU32LE(head + 32);
    if (frame_us < 20000 || frame_us > 500000) frame_us = 66666;   // default ~15fps

    // Locate the movi data start (should be kAviMoviFallback with our writer)
    size_t movi_start = kAviMoviFallback;
    for (size_t i = 12; i + 8 <= kAviHeadSize; i++) {
        if (memcmp(head + i, "movi", 4) == 0) {
            movi_start = i + 4;
            break;
        }
    }

    // ---- fetch the tail and scan backwards for idx1 ----
    size_t tail_len = op_file_size_ < kIdxTailCap ? op_file_size_ : kIdxTailCap;
    uint8_t* tail = PsramAlloc(tail_len);
    if (!tail) {
        ESP_LOGE(TAG, "No memory for idx1");
        return;
    }
    if (HttpGet(url_path, (long)(op_file_size_ - tail_len), (long)(op_file_size_ - 1),
                tail, tail_len, &received, 15000) == 0 || received < tail_len) {
        ESP_LOGE(TAG, "Failed to fetch AVI tail");
        heap_caps_free(tail);
        return;
    }
    size_t idx_pos = 0;
    bool idx_found = false;
    for (size_t i = tail_len - 4;; i--) {
        if (memcmp(tail + i, "idx1", 4) == 0) {
            idx_pos = i;
            idx_found = true;
            break;
        }
        if (i == 0) break;
    }
    if (!idx_found || idx_pos + 8 > tail_len) {
        ESP_LOGE(TAG, "%s: no idx1 chunk (file truncated?)", op_path_);
        heap_caps_free(tail);
        return;
    }
    uint32_t idx_size = ReadU32LE(tail + idx_pos + 4);
    if (idx_pos + 8 + idx_size > tail_len) {
        ESP_LOGE(TAG, "%s: idx1 (%u B) larger than fetched tail window", op_path_, (unsigned)idx_size);
        heap_caps_free(tail);
        return;
    }
    size_t frame_count = idx_size / 16;
    ESP_LOGI(TAG, "Replaying %s: %u frames @ %.1f fps", op_path_,
             (unsigned)frame_count, 1e6 / frame_us);

    // ---- frame loop ----
    uint8_t* jpg = PsramAlloc(kJpegCap);
    if (!jpg) {
        ESP_LOGE(TAG, "No memory for frames");
        heap_caps_free(tail);
        return;
    }
    const TickType_t interval = pdMS_TO_TICKS(frame_us / 1000);
    TickType_t next = xTaskGetTickCount();
    int consecutive_errors = 0;

    for (size_t f = 0; f < frame_count && !stop_requested_.load(); f++) {
        const uint8_t* entry = tail + idx_pos + 8 + f * 16;
        if (memcmp(entry, "00dc", 4) != 0) continue;
        uint32_t off = ReadU32LE(entry + 8);
        uint32_t sz = ReadU32LE(entry + 12);
        if (sz == 0 || sz > kJpegCap) {
            if (++consecutive_errors >= 10) break;
            continue;
        }
        size_t file_off = movi_start + off + 8;   // skip '00dc' + size
        if (HttpGet(url_path, (long)file_off, (long)(file_off + sz - 1),
                    jpg, kJpegCap, &received) == 0 || received < sz) {
            if (++consecutive_errors >= 10) break;
            continue;
        }
        uint8_t* rgb = nullptr;
        int w = 0, h = 0, rgb_len = 0;
        if (!DecodeJpeg(jpg, received, &rgb, &w, &h, &rgb_len)) {
            if (++consecutive_errors >= 10) break;
            continue;
        }
        consecutive_errors = 0;
        DisplayFrame(rgb, rgb_len, w, h);   // restarts the display preview timer
        vTaskDelayUntil(&next, interval);
    }

    heap_caps_free(jpg);
    heap_caps_free(tail);
    ESP_LOGI(TAG, "Replay %s (%s)", stop_requested_.load() ? "stopped" : "finished", op_path_);
    RestoreEyes();
}


// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

std::string WalleCamViewer::StartOp(Op op, const std::string& path, size_t file_size) {
    if (busy_.load()) {
        return "viewer busy (a preview/replay is already running)";
    }
    strlcpy(op_path_, path.c_str(), sizeof(op_path_));
    op_file_size_ = file_size;
    op_ = op;
    stop_requested_.store(false);
    busy_.store(true);
    BaseType_t ret = xTaskCreate(WorkerEntry, "cam_view", 8192, this, 4, &worker_);
    if (ret != pdPASS) {
        busy_.store(false);
        worker_ = nullptr;
        return "failed to start viewer task";
    }
    return "";
}

std::string WalleCamViewer::ShowLatestPhoto() {
    char path[64];
    size_t size = 0;
    std::string err = FindLatestOnCam("/photos/", path, sizeof(path), &size);
    if (!err.empty()) return err;
    err = StartOp(Op::kPhoto, path, size);
    if (err.empty()) ESP_LOGI(TAG, "Previewing latest photo %s", path);
    return err;
}

std::string WalleCamViewer::ShowPhoto(const std::string& path) {
    return StartOp(Op::kPhoto, path, 0);
}

std::string WalleCamViewer::PlayLatestVideo() {
    char path[64];
    size_t size = 0;
    std::string err = FindLatestOnCam("/videos/", path, sizeof(path), &size);
    if (!err.empty()) return err;
    if (size == 0) return "video file size unknown";
    err = StartOp(Op::kVideo, path, size);
    if (err.empty()) ESP_LOGI(TAG, "Replaying latest video %s", path);
    return err;
}

void WalleCamViewer::StopPlayback() {
    if (busy_.load()) {
        stop_requested_.store(true);
    }
}
