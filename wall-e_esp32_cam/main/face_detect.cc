/**
 * CAM-SIDE FACE / BODY DETECTION — implementation
 *
 * @file    face_detect.cc
 * @brief   ESP-WHO detection pipeline + HSV colour matching + EVT TRACK output.
 *
 * Detection flow (every ~200ms):
 *   1. JPEG decode to QVGA RGB565 (esp_new_jpeg)
 *   2. Run face detection (ESP-WHO MobileNet-SSD, always on)
 *   3. If face found → send EVT TRACK 1
 *   4. If no face AND locked → run body detection → colour-match → EVT TRACK 2
 *   5. If nothing → send EVT TRACK 0
 *
 * LOCK command flow:
 *   1. Run face detection on current frame
 *   2. Take largest face → compute torso ROI below it
 *   3. Extract 8×8 HSV 2D histogram → L2-normalise → store as reference
 *   4. Reply OK LOCK
 *
 * Dependencies: esp-who (face/person detect models), esp_new_jpeg (decode),
 *               esp-dl (inference runtime).
 */

#include "face_detect.h"
#include "camera_pins.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_camera.h>
#include <driver/uart.h>
#include <esp_jpeg_dec.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "human_face_detect.hpp"
#include "dl_image.hpp"

static const char* TAG = "face_detect";

// ---- Configuration --------------------------------------------------
#define DETECT_INTERVAL_MS      200     // run detection every 200ms (5 Hz)
#define DETECT_MIN_CONFIDENCE    50     // minimum detection score (0-100)
#define HIST_BINS_H              8      // HSV H bins
#define HIST_BINS_S              8      // HSV S bins
#define HIST_MATCH_THRESHOLD    60     // histogram intersection threshold (0-100)
#define TORSO_Y_RATIO           1.2f   // torso ROI starts at face_bottom * TORSO_Y_RATIO
#define TORSO_H_RATIO           1.5f   // torso ROI height = face_h * TORSO_H_RATIO

// UART output (same port as cam_link)
#define FACE_UART_PORT          UART_NUM_1

// ---- Detection state ------------------------------------------------
static bool s_initialised = false;
static bool s_locked = false;
static int64_t s_last_detect_us = 0;

// Colour reference (captured at LOCK time)
static float s_ref_hist[HIST_BINS_H * HIST_BINS_S] = {0};
static bool  s_has_ref = false;

// Model object (human_face_detect package from ESP component registry)
static HumanFaceDetect* s_face_detector = nullptr;

// Hysteresis: require N consecutive detections/misses to switch state.
static int  s_face_hit_count = 0;   // consecutive frames with a face
static int  s_face_miss_count = 0;  // consecutive frames without a face
static bool s_face_active = false;  // current stable state
static constexpr int kHysteresis = 2;  // frames before switching

// Temporary decode buffer (QVGA RGB565 = 320*240*2 = 153600 bytes)
static uint8_t* s_rgb_buf = nullptr;
static constexpr int kQvgaW = 320;
static constexpr int kQvgaH = 240;
static constexpr size_t kRgbBufSize = kQvgaW * kQvgaH * 2;

// ====================================================================
// Helpers
// ====================================================================

/// Send an EVT TRACK line over UART.
static void send_track(int type, int x, int y, int w, int h, int conf) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "EVT TRACK %d %d %d %d %d %d\n",
                       type, x, y, w, h, conf);
    uart_write_bytes(FACE_UART_PORT, buf, len);
}

/// Convert RGB565 pixel to HSV and accumulate into a 2D histogram.
/// H: 0..7 (hue sector), S: 0..7 (saturation level). V (value) is ignored.
static void rgb565_to_hist_bin(uint16_t rgb, int& h_bin, int& s_bin) {
    int r = (rgb >> 11) & 0x1F;   // 5 bits
    int g = (rgb >> 5)  & 0x3F;   // 6 bits
    int b =  rgb        & 0x1F;   // 5 bits

    // Scale to 0-255
    r = (r * 255) / 31;
    g = (g * 255) / 63;
    b = (b * 255) / 31;

    int max_c = std::max({r, g, b});
    int min_c = std::min({r, g, b});
    int delta = max_c - min_c;

    // Hue (0-360 mapped to 0-7)
    int h = 0;
    if (delta > 0) {
        if (max_c == r) {
            h = 60 * (g - b) / delta;
        } else if (max_c == g) {
            h = 120 + 60 * (b - r) / delta;
        } else {
            h = 240 + 60 * (r - g) / delta;
        }
    }
    if (h < 0) h += 360;
    h_bin = (h * HIST_BINS_H) / 360;
    if (h_bin >= HIST_BINS_H) h_bin = HIST_BINS_H - 1;

    // Saturation (0-255 mapped to 0-7)
    int s = max_c > 0 ? (delta * 255) / max_c : 0;
    s_bin = (s * HIST_BINS_S) / 256;
    if (s_bin >= HIST_BINS_S) s_bin = HIST_BINS_S - 1;
}

/// Extract torso HSV histogram from a face bbox.
/// Torso region: starts below face, ~1.5× face height, center 60% width.
static void extract_torso_histogram(const uint8_t* rgb565, int fw, int fh,
                                     int face_x, int face_y, int face_w, int face_h,
                                     float* hist_out) {
    memset(hist_out, 0, HIST_BINS_H * HIST_BINS_S * sizeof(float));

    int tx = face_x + face_w / 5;           // center 60% width
    int tw = face_w * 3 / 5;
    int ty = face_y + face_h;               // start below face
    int th = face_h * 1.5f;
    if (ty + th > fh) th = fh - ty;
    if (tx < 0) { tw += tx; tx = 0; }
    if (tx + tw > fw) tw = fw - tx;
    if (ty < 0) { th += ty; ty = 0; }
    if (tw <= 0 || th <= 0) return;

    int samples = 0;
    for (int row = ty; row < ty + th; row += 2) {       // subsample
        for (int col = tx; col < tx + tw; col += 2) {
            int idx = (row * fw + col) * 2;
            uint16_t pixel = rgb565[idx] | (rgb565[idx + 1] << 8);
            int hb, sb;
            rgb565_to_hist_bin(pixel, hb, sb);
            hist_out[hb * HIST_BINS_S + sb] += 1.0f;
            samples++;
        }
    }

    // L2 normalise
    if (samples > 0) {
        float sum_sq = 0;
        for (int i = 0; i < HIST_BINS_H * HIST_BINS_S; i++) {
            sum_sq += hist_out[i] * hist_out[i];
        }
        if (sum_sq > 0) {
            float inv_norm = 1.0f / sqrtf(sum_sq);
            for (int i = 0; i < HIST_BINS_H * HIST_BINS_S; i++) {
                hist_out[i] *= inv_norm;
            }
        }
    }
}

/// Histogram intersection: sum(min(a_i, b_i)). Returns 0-100.
static int histogram_intersect(const float* a, const float* b) {
    float score = 0;
    for (int i = 0; i < HIST_BINS_H * HIST_BINS_S; i++) {
        score += std::min(a[i], b[i]);
    }
    return (int)(score * 100.0f);   // L2-normalised → max intersect = 1.0
}

// ====================================================================
// JPEG → RGB565 decode (QVGA)
// ====================================================================

static bool decode_jpeg_qvga(const uint8_t* jpeg_buf, size_t jpeg_len) {
    if (!s_rgb_buf) {
        // jpeg_calloc_align ensures 16-byte alignment required by the HW decoder
        s_rgb_buf = (uint8_t*)jpeg_calloc_align(kRgbBufSize, 16);
        if (!s_rgb_buf) {
            ESP_LOGE(TAG, "Failed to allocate QVGA decode buffer");
            return false;
        }
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.scale.width = kQvgaW;
    cfg.scale.height = kQvgaH;

    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;

    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(jpeg_buf);
    io.inbuf_len = (int)jpeg_len;
    io.outbuf = s_rgb_buf;

    // Parse header first — required before jpeg_dec_process
    jpeg_dec_header_info_t info;
    bool ok = false;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK) {
        ok = (jpeg_dec_process(dec, &io) == JPEG_ERR_OK);
    }
    jpeg_dec_close(dec);
    return ok;
}

// ====================================================================
// Public API
// ====================================================================

int face_detect_init() {
    if (s_initialised) return 0;

    // Allocate decode buffer
    if (!s_rgb_buf) {
        s_rgb_buf = (uint8_t*)jpeg_calloc_align(kRgbBufSize, 16);
        if (!s_rgb_buf) {
            ESP_LOGE(TAG, "No PSRAM for decode buffer");
            return -1;
        }
    }

    // Load the face detection model (MobileNet-SSD, quantized for ESP32-S3)
    s_face_detector = new HumanFaceDetect(
        static_cast<HumanFaceDetect::model_type_t>(0),  // default model (MOBILE_NET_SSD)
        false  // allocate in PSRAM via malloc (not internal SRAM)
    );
    if (!s_face_detector) {
        ESP_LOGE(TAG, "Failed to create face detector");
        return -1;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "Face detect ready (QVGA %dx%d, interval %d ms)",
             kQvgaW, kQvgaH, DETECT_INTERVAL_MS);
    return 0;
}

void face_detect_process(const uint8_t* jpeg_buf, size_t jpeg_len) {
    if (!s_initialised) return;

    // Rate limit
    int64_t now = esp_timer_get_time();
    if (now - s_last_detect_us < DETECT_INTERVAL_MS * 1000LL) return;
    s_last_detect_us = now;

    // Decode
    if (!decode_jpeg_qvga(jpeg_buf, jpeg_len)) return;

    // ---- Stage 1: Face detection (always on) ----
    bool face_found = false;
    int best_x = 0, best_y = 0, best_w = 0, best_h = 0, best_conf = 0;

    if (s_face_detector) {
        dl::image::img_t img(s_rgb_buf, kQvgaW, kQvgaH, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE);
        auto& faces = s_face_detector->run(img);

        if (!faces.empty()) {
            // Pick the largest face above confidence threshold
            // result_t::box = [left, top, right, bottom]
            for (const auto& f : faces) {
                int w = f.box[2] - f.box[0];
                int h = f.box[3] - f.box[1];
                int area = w * h;
                int conf = (int)(f.score * 100);
                if (conf >= DETECT_MIN_CONFIDENCE && area > best_w * best_h) {
                    best_x = f.box[0]; best_y = f.box[1];
                    best_w = w; best_h = h;
                    best_conf = conf;
                    face_found = true;
                }
            }
        }
    }

    // ---- Hysteresis: prevent rapid face/no-face oscillation ----
    if (face_found) {
        s_face_hit_count++;
        s_face_miss_count = 0;
        if (s_face_hit_count >= kHysteresis) {
            s_face_active = true;
            s_face_hit_count = kHysteresis;  // clamp
        }
    } else {
        s_face_miss_count++;
        s_face_hit_count = 0;
        if (s_face_miss_count >= kHysteresis) {
            s_face_active = false;
            s_face_miss_count = kHysteresis;
        }
    }

    // Only emit when the stable state is "face found"
    if (s_face_active && face_found) {
        send_track(1, best_x, best_y, best_w, best_h, best_conf);
        return;
    }

    // ---- Stage 2: Body detection (only when locked, fallback) ----
    if (!s_locked || !s_has_ref) {
        send_track(0, 0, 0, 0, 0, 0);
        return;
    }

    // TODO: Replace with real ESP-WHO person detection
    // auto& bodies = s_body_detector->detect(s_rgb_buf, kQvgaW, kQvgaH);

    bool body_matched = false;
    best_x = best_y = best_w = best_h = best_conf = 0;

    // for (const auto& b : bodies) {
    //     int conf = (int)(b.confidence * 100);
    //     if (conf < DETECT_MIN_CONFIDENCE) continue;
    //
    //     // Colour-match: extract torso HSV, compare to reference
    //     float hist[HIST_BINS_H * HIST_BINS_S];
    //     extract_torso_histogram(s_rgb_buf, kQvgaW, kQvgaH,
    //                             b.bbox.x, b.bbox.y, b.bbox.width, b.bbox.height,
    //                             hist);
    //     int match = histogram_intersect(hist, s_ref_hist);
    //     if (match >= HIST_MATCH_THRESHOLD) {
    //         best_x = b.bbox.x; best_y = b.bbox.y;
    //         best_w = b.bbox.width; best_h = b.bbox.height;
    //         best_conf = (conf + match) / 2;  // blend detection + colour scores
    //         body_matched = true;
    //         break;  // take the first match
    //     }
    // }

    if (body_matched) {
        send_track(2, best_x, best_y, best_w, best_h, best_conf);
    } else {
        send_track(0, 0, 0, 0, 0, 0);
    }
}

int face_detect_lock() {
    if (!s_initialised || !s_face_detector) return -1;

    // Grab a fresh frame from the camera
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return -2;

    if (!decode_jpeg_qvga(fb->buf, fb->len)) {
        esp_camera_fb_return(fb);
        return -3;
    }
    esp_camera_fb_return(fb);

    // Run face detection to find the person to lock onto
    dl::image::img_t img(s_rgb_buf, kQvgaW, kQvgaH, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE);
    auto& faces = s_face_detector->run(img);
    if (faces.empty()) return -4;  // no face to lock onto

    // Take the largest face
    auto& best = faces.front();
    int best_area = (best.box[2] - best.box[0]) * (best.box[3] - best.box[1]);
    for (const auto& f : faces) {
        int area = (f.box[2] - f.box[0]) * (f.box[3] - f.box[1]);
        if (area > best_area) { best = f; best_area = area; }
    }

    // Capture torso colour reference for body-detection fallback
    extract_torso_histogram(s_rgb_buf, kQvgaW, kQvgaH,
                            best.box[0], best.box[1],
                            best.box[2] - best.box[0], best.box[3] - best.box[1],
                            s_ref_hist);

    s_has_ref = true;
    s_locked = true;
    ESP_LOGI(TAG, "Person locked (colour reference captured)");
    return 0;
}

int face_detect_unlock() {
    s_locked = false;
    s_has_ref = false;
    memset(s_ref_hist, 0, sizeof(s_ref_hist));
    ESP_LOGI(TAG, "Person unlocked (reference cleared)");
    return 0;
}

bool face_detect_is_locked() {
    return s_locked;
}
