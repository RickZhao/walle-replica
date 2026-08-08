/**
 * WALL-E CAMERA MODULE (ESP32-S3-CAM) — ESP-IDF port
 *
 * @file    cam_main.cpp
 * @brief   Standalone MJPEG streaming firmware for the ESP32-S3-CAM
 *          module, ported from Arduino to ESP-IDF for unified builds.
 *
 * Endpoints (same as original):
 *   /        -> plain-text status page
 *   /stream  -> MJPEG stream
 *   /eyes    -> eye expression: /eyes?expr=neutral|sad|left|right
 *   /capture -> photo to SD (/photos/IMG_nnnn.jpg)
 *   /record?action=start|stop -> MJPEG AVI video
 *   /files   -> JSON list of photos/videos
 *   /file?path=... -> download (GET) / delete (DELETE)
 *
 * UART link: CAM_PROTOCOL v1 on UART1, 115200 8N1, RX=14 TX=21
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_jpeg_dec.h"

#include "camera_pins.h"

static const char *TAG = "cam";

// ====================================================================
// Configuration
// ====================================================================

#define CAM_WIFI_SSID      ""
#define CAM_WIFI_PASSWORD  ""
#define WALLE_AP_SSID      "WallE"
#define WALLE_AP_PASSWORD  "walle1234"

#define CAM_FRAME_SIZE     FRAMESIZE_VGA
#define CAM_JPEG_QUALITY   12

#define CAM_SD_ENABLED     1
#define SD_CS_PIN          2
#define SD_SCK_PIN         7
#define SD_MISO_PIN        8
#define SD_MOSI_PIN        9
#define REC_FPS            15

#define EYE_DISPLAYS_ENABLED 1
#define EYE_SPI_HOST        SPI3_HOST
#define EYE_SPI_SCK         42
#define EYE_SPI_MOSI        45
#define EYE_SPI_DC          41
#define EYE_SPI_RST         46
#define EYE_L_CS            2
#define EYE_R_CS            0

#define CAM_LINK_UART        UART_NUM_1
#define CAM_LINK_RX_PIN      14
#define CAM_LINK_TX_PIN      21
#define CAM_LINK_MAX_LINE    256

#define CAM_FW_VERSION       "1.2.0"
#define CAM_PROTO_VERSION    1

// ====================================================================
// Eye display types and globals
// ====================================================================

enum EyeExpression {
    EYE_NEUTRAL,
    EYE_SAD,
    EYE_TILT_LEFT,
    EYE_TILT_RIGHT,
};

// Eye geometry (240x240 display)
#define EYE_CX       120
#define EYE_CY       120
#define LENS_W       170
#define LENS_H       130
#define LENS_RADIUS  40
#define IRIS_RADIUS  38
#define GAZE_DX      22
#define GAZE_DY_SAD  18

// RGB565 colors
#define COL_LENS     0xE71C
#define COL_IRIS     0x2965
#define COL_PUPIL    0x1082
#define COL_HILIGHT  0xFFFF
#define COL_WHITE     0xFFFF
#define COL_BLACK     0x0000

#define BLINK_MIN_MS 2500
#define BLINK_MAX_MS 6000
#define BLINK_STEP_MS 40
#define BLINK_STEPS   4

#if EYE_DISPLAYS_ENABLED
static esp_lcd_panel_io_handle_t eye_io_L = nullptr;
static esp_lcd_panel_io_handle_t eye_io_R = nullptr;
static esp_lcd_panel_handle_t eye_panel_L = nullptr;
static esp_lcd_panel_handle_t eye_panel_R = nullptr;
static uint16_t *eye_fb = nullptr;  // 240x240 RGB565 framebuffer (115200 bytes)
#endif

static EyeExpression eye_expr = EYE_NEUTRAL;
static float eye_lid_close = 0.0f;
static int eye_blink_phase = 0;
static int64_t eye_next_blink_at = 3000000;  // us
static int64_t eye_next_blink_frame_at = 0;
static bool eye_overlay_active = false;

// ====================================================================
// SD card / recording globals
// ====================================================================

#if CAM_SD_ENABLED
static bool sd_ready = false;
static volatile bool recording = false;
static volatile bool record_stop_request = false;
static volatile bool record_abnormal = false;
static TaskHandle_t record_task = nullptr;
static char record_path[40];
static volatile uint32_t record_frames = 0;
static int64_t record_start_us = 0;
#endif

// ====================================================================
// AVI Writer (uses C FILE*)
// ====================================================================

class AviWriter {
public:
    bool Begin(FILE *file, uint16_t width, uint16_t height, uint16_t fps) {
        f_ = file;
        w_ = width; h_ = height; fps_ = fps;
        frame_count_ = 0;
        index_.clear();

        WriteTag("RIFF"); WriteU32(0); WriteTag("AVI ");
        WriteTag("LIST"); WriteU32(192); WriteTag("hdrl");
        WriteTag("avih"); WriteU32(56);
        WriteU32(1000000UL / fps_);
        WriteU32(0); WriteU32(0); WriteU32(0x10);
        avih_total_frames_pos_ = ftell(f_);
        WriteU32(0); WriteU32(0); WriteU32(1); WriteU32(0);
        WriteU32(w_); WriteU32(h_);
        WriteU32(0); WriteU32(0); WriteU32(0); WriteU32(0);

        WriteTag("LIST"); WriteU32(116); WriteTag("strl");
        WriteTag("strh"); WriteU32(56);
        WriteTag("vids"); WriteTag("MJPG");
        WriteU32(0); WriteU32(0); WriteU32(0);
        WriteU32(1); WriteU32(fps_); WriteU32(0);
        strh_length_pos_ = ftell(f_);
        WriteU32(0); WriteU32(0); WriteU32(0xFFFFFFFF); WriteU32(0);
        WriteU16(0); WriteU16(0); WriteU16(w_); WriteU16(h_);

        WriteTag("strf"); WriteU32(40);
        WriteU32(40); WriteU32(w_); WriteU32(h_);
        WriteU16(1); WriteU16(24); WriteTag("MJPG");
        WriteU32((uint32_t)w_ * h_ * 3);
        WriteU32(0); WriteU32(0); WriteU32(0); WriteU32(0);

        WriteTag("LIST");
        movi_size_pos_ = ftell(f_);
        WriteU32(0);
        WriteTag("movi");
        movi_data_pos_ = ftell(f_);
        return ftell(f_) > 0;
    }

    bool AddFrame(const uint8_t *jpeg, size_t len) {
        if (!f_ || len == 0) return false;
        index_.push_back((uint32_t)(ftell(f_) - movi_data_pos_));
        index_.push_back((uint32_t)len);
        WriteTag("00dc");
        WriteU32((uint32_t)len);
        size_t written = fwrite(jpeg, 1, len, f_);
        if (len & 1) fputc(0, f_);
        frame_count_++;
        return written == len;
    }

    bool Finalize() {
        if (!f_) return false;
        WriteTag("idx1");
        WriteU32((uint32_t)index_.size() * 4);
        for (size_t i = 0; i + 1 < index_.size(); i += 2) {
            WriteTag("00dc");
            WriteU32(0x10);
            WriteU32(index_[i]);
            WriteU32(index_[i + 1]);
        }
        uint32_t file_size = ftell(f_);
        PatchU32(4, file_size - 8);
        PatchU32(avih_total_frames_pos_, frame_count_);
        PatchU32(strh_length_pos_, frame_count_);
        PatchU32(movi_size_pos_, 4 + (file_size - idx1Size()) - movi_data_pos_);
        fflush(f_);
        return true;
    }

    uint32_t Frames() const { return frame_count_; }

private:
    uint32_t idx1Size() const { return 8 + frame_count_ * 16; }

    void WriteTag(const char *tag) { fwrite(tag, 1, 4, f_); }
    void WriteU16(uint16_t v) {
        uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        fwrite(b, 1, 2, f_);
    }
    void WriteU32(uint32_t v) {
        uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                         (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
        fwrite(b, 1, 4, f_);
    }
    void PatchU32(size_t pos, uint32_t v) {
        size_t cur = ftell(f_);
        fseek(f_, pos, SEEK_SET);
        WriteU32(v);
        fseek(f_, cur, SEEK_SET);
    }

    FILE *f_ = nullptr;
    uint16_t w_ = 0, h_ = 0, fps_ = 15;
    uint32_t frame_count_ = 0;
    size_t movi_data_pos_ = 0, movi_size_pos_ = 0;
    size_t avih_total_frames_pos_ = 0, strh_length_pos_ = 0;
    std::vector<uint32_t> index_;
};

// ====================================================================
// Eye display: drawing primitives on RGB565 framebuffer
// ====================================================================

#if EYE_DISPLAYS_ENABLED

static void fbFillScreen(uint16_t color) {
    for (int i = 0; i < 240 * 240; i++) eye_fb[i] = color;
}

static void fbFillRect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + h > 240) h = 240 - y;
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            eye_fb[row * 240 + col] = color;
        }
    }
}

static void fbFillCircle(int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx_max = (int)sqrtf((float)(r * r - dy * dy));
        int y = cy + dy;
        if (y < 0 || y >= 240) continue;
        int x0 = cx - dx_max, x1 = cx + dx_max;
        if (x0 < 0) x0 = 0;
        if (x1 >= 240) x1 = 239;
        for (int x = x0; x <= x1; x++) eye_fb[y * 240 + x] = color;
    }
}

static void fbFillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
    fbFillRect(x + r, y, w - 2 * r, h, color);
    fbFillRect(x, y + r, w, h - 2 * r, color);
    fbFillCircle(x + r, y + r, r, color);
    fbFillCircle(x + w - r - 1, y + r, r, color);
    fbFillCircle(x + r, y + h - r - 1, r, color);
    fbFillCircle(x + w - r - 1, y + h - r - 1, r, color);
}

static void fbFillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, uint16_t color) {
    // Bounding box
    int min_y = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
    int max_y = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);
    if (min_y < 0) min_y = 0;
    if (max_y >= 240) max_y = 239;

    auto orient = [](int ax, int ay, int bx, int by, int px, int py) -> int {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };

    for (int y = min_y; y <= max_y; y++) {
        int min_x = 240, max_x = -1;
        for (int x = 0; x < 240; x++) {
            int o1 = orient(x1, y1, x2, y2, x, y);
            int o2 = orient(x2, y2, x3, y3, x, y);
            int o3 = orient(x3, y3, x1, y1, x, y);
            bool inside = (o1 >= 0 && o2 >= 0 && o3 >= 0) ||
                         (o1 <= 0 && o2 <= 0 && o3 <= 0);
            if (inside) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
            }
        }
        if (min_x <= max_x) {
            for (int x = min_x; x <= max_x; x++) {
                eye_fb[y * 240 + x] = color;
            }
        }
    }
}

static void fbFlush() {
    esp_lcd_panel_draw_bitmap(eye_panel_L, 0, 0, 239, 239, eye_fb);
    esp_lcd_panel_draw_bitmap(eye_panel_R, 0, 0, 239, 239, eye_fb);
}

static void drawEye(EyeExpression expr, float lid) {
    fbFillScreen(COL_BLACK);

    fbFillRoundRect(EYE_CX - LENS_W / 2, EYE_CY - LENS_H / 2,
                    LENS_W, LENS_H, LENS_RADIUS, COL_LENS);

    int dx = 0, dy = 0;
    if (expr == EYE_TILT_LEFT)  dx = -GAZE_DX;
    if (expr == EYE_TILT_RIGHT) dx =  GAZE_DX;
    if (expr == EYE_SAD)        dy =  GAZE_DY_SAD;

    fbFillCircle(EYE_CX + dx, EYE_CY + dy, IRIS_RADIUS, COL_IRIS);
    fbFillCircle(EYE_CX + dx, EYE_CY + dy, IRIS_RADIUS / 2, COL_PUPIL);
    fbFillCircle(EYE_CX + dx - 10, EYE_CY + dy - 10, 7, COL_HILIGHT);

    if (expr == EYE_SAD) {
        fbFillTriangle(EYE_CX - LENS_W / 2, EYE_CY - LENS_H / 2,
                       EYE_CX + LENS_W / 2, EYE_CY - LENS_H / 2,
                       EYE_CX + LENS_W / 2, EYE_CY - 20,
                       COL_BLACK);
    }

    if (lid > 0.0f) {
        int bar = (int)((LENS_H / 2 + 10) * lid);
        fbFillRect(0, 0, 240, EYE_CY - LENS_H / 2 + bar, COL_BLACK);
        fbFillRect(0, EYE_CY + LENS_H / 2 - bar, 240, 240 - (EYE_CY + LENS_H / 2 - bar), COL_BLACK);
    }

    fbFlush();
}

// 7-segment digit for countdown
static const uint8_t DIGIT_SEGMENTS[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void drawDigit(int digit) {
    const int W = 90, H_ = 150, T = 16;
    const int X0 = EYE_CX - W / 2, Y0 = EYE_CY - H_ / 2;
    uint8_t seg = DIGIT_SEGMENTS[digit];
    fbFillScreen(COL_BLACK);
    if (seg & 0x01) fbFillRect(X0,         Y0,                 W, T,     COL_WHITE);
    if (seg & 0x02) fbFillRect(X0 + W - T, Y0,                 T, H_ / 2, COL_WHITE);
    if (seg & 0x04) fbFillRect(X0 + W - T, Y0 + H_ / 2,         T, H_ / 2, COL_WHITE);
    if (seg & 0x08) fbFillRect(X0,         Y0 + H_ - T,         W, T,     COL_WHITE);
    if (seg & 0x10) fbFillRect(X0,         Y0 + H_ / 2,         T, H_ / 2, COL_WHITE);
    if (seg & 0x20) fbFillRect(X0,         Y0,                 T, H_ / 2, COL_WHITE);
    if (seg & 0x40) fbFillRect(X0,         Y0 + H_ / 2 - T / 2, W, T,     COL_WHITE);
    fbFlush();
}

enum CamLinkState {
    LINK_IDLE = 0,
    LINK_PHOTO_PREPARE,
    LINK_PHOTO_COUNTDOWN,
    LINK_PHOTO_PREVIEW,
    LINK_SHOW,
};

// Forward declarations
static void redrawEyes() { drawEye(eye_expr, eye_lid_close); }

#endif // EYE_DISPLAYS_ENABLED

// ====================================================================
// Eye display public interface
// ====================================================================

static void eyeDisplayInit() {
#if EYE_DISPLAYS_ENABLED
    // Allocate framebuffer in PSRAM
    eye_fb = (uint16_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    assert(eye_fb);

    // SPI bus for eye displays
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = EYE_SPI_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.sclk_io_num = EYE_SPI_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 240 * 240 * 2 + 8;
    ESP_ERROR_CHECK(spi_bus_initialize(EYE_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Left eye panel IO
    esp_lcd_panel_io_spi_config_t io_cfg_L = {};
    io_cfg_L.cs_gpio_num = (gpio_num_t)EYE_L_CS;
    io_cfg_L.dc_gpio_num = (gpio_num_t)EYE_SPI_DC;
    io_cfg_L.spi_mode = 0;
    io_cfg_L.pclk_hz = 40 * 1000 * 1000;
    io_cfg_L.trans_queue_depth = 10;
    io_cfg_L.lcd_cmd_bits = 8;
    io_cfg_L.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EYE_SPI_HOST,
                                              &io_cfg_L, &eye_io_L));

    // Right eye panel IO
    esp_lcd_panel_io_spi_config_t io_cfg_R = {};
    io_cfg_R.cs_gpio_num = (gpio_num_t)EYE_R_CS;
    io_cfg_R.dc_gpio_num = (gpio_num_t)EYE_SPI_DC;
    io_cfg_R.spi_mode = 0;
    io_cfg_R.pclk_hz = 40 * 1000 * 1000;
    io_cfg_R.trans_queue_depth = 10;
    io_cfg_R.lcd_cmd_bits = 8;
    io_cfg_R.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EYE_SPI_HOST,
                                              &io_cfg_R, &eye_io_R));

    // GC9A01 panel configs
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = (gpio_num_t)EYE_SPI_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;

    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(eye_io_L, &panel_cfg, &eye_panel_L));
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(eye_io_R, &panel_cfg, &eye_panel_R));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(eye_panel_L));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(eye_panel_R));
    ESP_ERROR_CHECK(esp_lcd_panel_init(eye_panel_L));
    ESP_ERROR_CHECK(esp_lcd_panel_init(eye_panel_R));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(eye_panel_L, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(eye_panel_R, true));

    eye_expr = EYE_NEUTRAL;
    eye_lid_close = 0.0f;
    redrawEyes();
    ESP_LOGI(TAG, "Eye displays started (2x GC9A01, shared SPI SCK=%d MOSI=%d DC=%d RST=%d)",
             EYE_SPI_SCK, EYE_SPI_MOSI, EYE_SPI_DC, EYE_SPI_RST);
#endif
}

static void eyeDisplayLoop() {
#if EYE_DISPLAYS_ENABLED
    if (eye_overlay_active) return;
    int64_t now = esp_timer_get_time();
    if (eye_blink_phase == 0 && now >= eye_next_blink_at) {
        eye_blink_phase = 1;
        eye_next_blink_frame_at = now;
    }
    if (eye_blink_phase != 0 && now >= eye_next_blink_frame_at) {
        eye_next_blink_frame_at = now + BLINK_STEP_MS * 1000;
        if (eye_blink_phase == 1) {
            eye_lid_close += 1.0f / BLINK_STEPS;
            if (eye_lid_close >= 1.0f) { eye_lid_close = 1.0f; eye_blink_phase = 2; }
        } else {
            eye_lid_close -= 1.0f / BLINK_STEPS;
            if (eye_lid_close <= 0.0f) {
                eye_lid_close = 0.0f;
                eye_blink_phase = 0;
                eye_next_blink_at = now + (BLINK_MIN_MS + esp_random() % (BLINK_MAX_MS - BLINK_MIN_MS)) * 1000LL;
            }
        }
        redrawEyes();
    }
#endif
}

static void eyeDisplaySetExpression(EyeExpression expression) {
#if EYE_DISPLAYS_ENABLED
    if (expression == eye_expr) return;
    eye_expr = expression;
    redrawEyes();
#endif
}

static bool eyeDisplaySetByName(const char *name) {
    if (strcmp(name, "neutral") == 0)    { eyeDisplaySetExpression(EYE_NEUTRAL);    return true; }
    if (strcmp(name, "sad") == 0)        { eyeDisplaySetExpression(EYE_SAD);        return true; }
    if (strcmp(name, "left") == 0)       { eyeDisplaySetExpression(EYE_TILT_LEFT);  return true; }
    if (strcmp(name, "right") == 0)      { eyeDisplaySetExpression(EYE_TILT_RIGHT); return true; }
    return false;
}

static const char *eyeDisplayExpressionName() {
#if EYE_DISPLAYS_ENABLED
    switch (eye_expr) {
        case EYE_SAD:        return "sad";
        case EYE_TILT_LEFT:  return "left";
        case EYE_TILT_RIGHT: return "right";
        case EYE_NEUTRAL:
        default:             return "neutral";
    }
#else
    return "disabled";
#endif
}

static EyeExpression eyeDisplayGetExpression() {
#if EYE_DISPLAYS_ENABLED
    return eye_expr;
#else
    return EYE_NEUTRAL;
#endif
}

static void eyeDisplayFillScreen(uint16_t color) {
#if EYE_DISPLAYS_ENABLED
    eye_overlay_active = true;
    fbFillScreen(color);
    fbFlush();
#endif
}

static void eyeDisplayShowNumber(int digit) {
#if EYE_DISPLAYS_ENABLED
    if (digit < 0 || digit > 9) return;
    eye_overlay_active = true;
    drawDigit(digit);
#endif
}

static void eyeDisplayResume() {
#if EYE_DISPLAYS_ENABLED
    eye_overlay_active = false;
    redrawEyes();
#endif
}

// ====================================================================
// UART cam link (CAM_PROTOCOL v1)
// ====================================================================

static CamLinkState link_state = LINK_IDLE;
static int64_t link_state_at = 0;
static int link_countdown_digit = -1;
static EyeExpression link_saved_expr = EYE_NEUTRAL;
static char link_line[CAM_LINK_MAX_LINE];
static size_t link_line_len = 0;

static void linkSend(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[len++] = '\n';
    uart_write_bytes(CAM_LINK_UART, buf, len);
}

void camLinkNotifyRecDone(const char *path, uint32_t frames) {
    char buf[128];
    snprintf(buf, sizeof(buf), "EVT REC_DONE %s %lu\n", path, (unsigned long)frames);
    uart_write_bytes(CAM_LINK_UART, buf, strlen(buf));
}

static void linkAbortFlow() {
    link_state = LINK_IDLE;
    eyeDisplaySetExpression(link_saved_expr);
    eyeDisplayResume();
}

// SD helpers
static void FrameDims(uint16_t *w, uint16_t *h) {
    switch (CAM_FRAME_SIZE) {
        case FRAMESIZE_QVGA: *w = 320;  *h = 240;  break;
        case FRAMESIZE_SVGA: *w = 800;  *h = 600;  break;
        case FRAMESIZE_XGA:  *w = 1024; *h = 768;  break;
        case FRAMESIZE_UXGA: *w = 1600; *h = 1200; break;
        case FRAMESIZE_VGA:
        default:             *w = 640;  *h = 480;  break;
    }
}

static void NextSdPath(char *out, size_t size, const char *dir, const char *prefix, const char *ext) {
    for (int i = 1; i < 10000; i++) {
        snprintf(out, size, "/sdcard%s/%s_%04d%s", dir, prefix, i, ext);
        FILE *f = fopen(out, "r");
        if (!f) return;
        fclose(f);
    }
}

static bool SanitizeSdPath(const char *in, char *out, size_t out_size) {
    if (strlen(in) > out_size - 1) return false;
    if (strstr(in, "..")) return false;
    if (strncmp(in, "/photos/", 8) != 0 && strncmp(in, "/videos/", 8) != 0) return false;
    snprintf(out, out_size, "/sdcard%s", in);
    return true;
}

#if CAM_SD_ENABLED

static void recordTaskFunc(void *);

static bool RecordStart(char *out_path, size_t out_size) {
    if (recording) return false;
    NextSdPath(record_path, sizeof(record_path), "/videos", "VID", ".avi");
    record_frames = 0;
    record_start_us = esp_timer_get_time();
    record_stop_request = false;
    record_abnormal = false;
    recording = true;
    if (xTaskCreate(recordTaskFunc, "cam_record", 4096, nullptr, 5, &record_task) != pdPASS) {
        recording = false;
        return false;
    }
    strncpy(out_path, record_path, out_size);
    out_path[out_size - 1] = 0;
    return true;
}

static int32_t RecordStop() {
    if (!recording) return -1;
    record_stop_request = true;
    for (int i = 0; i < 300 && recording; i++) vTaskDelay(pdMS_TO_TICKS(10));
    return (int32_t)record_frames;
}

// JPEG decode for eye preview
static bool LinkShowJpegFile(const char *path);

static bool LinkLatestPhoto(char *out, size_t size) {
    if (!sd_ready) return false;
    DIR *d = opendir("/sdcard/photos");
    if (!d) return false;
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_type == DT_REG) {
            char full[48];
            snprintf(full, sizeof(full), "/sdcard/photos/%.31s", ent->d_name);
            if (!found || strcmp(full, out) > 0) {
                strncpy(out, full, size);
                out[size - 1] = 0;
            }
            found = true;
        }
    }
    closedir(d);
    return found;
}

static void linkPhotoCapture() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { linkSend("ERR IO"); linkAbortFlow(); return; }

    char path[40];
    NextSdPath(path, sizeof(path), "/photos", "IMG", ".jpg");
    FILE *f = fopen(path, "wb");
    if (!f) { esp_camera_fb_return(fb); linkSend("ERR SD"); linkAbortFlow(); return; }
    size_t written = fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    esp_camera_fb_return(fb);

    if (written == 0) { linkSend("ERR SD"); linkAbortFlow(); return; }

    // Strip /sdcard prefix for the response (protocol uses /photos/... paths)
    const char *name = strrchr(path, '/') + 1;
    linkSend("OK FILE /photos/%s", name);

    if (!LinkShowJpegFile(path)) { linkAbortFlow(); return; }
    link_state = LINK_PHOTO_PREVIEW;
    link_state_at = esp_timer_get_time();
}

#define PHOTO_PREPARE_MS   500
#define PHOTO_COUNTDOWN_MS 3000
#define LINK_PREVIEW_MS    5000

// JPEG preview decode to both eye displays
static bool LinkShowJpegFile(const char *path) {
#if EYE_DISPLAYS_ENABLED
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "Show: cannot open %s", path); return false; }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size == 0) { fclose(f); return false; }

    uint8_t *jpeg_buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf) { fclose(f); return false; }
    fread(jpeg_buf, 1, size, f);
    fclose(f);

    // Decode JPEG to RGB565 framebuffer using esp_new_jpeg
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        free(jpeg_buf);
        return false;
    }

    jpeg_dec_io_t io = {};
    io.inbuf = jpeg_buf;
    io.inbuf_len = (int)size;
    io.inbuf_remain = 0;

    jpeg_dec_header_info_t info;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        jpeg_dec_close(dec);
        free(jpeg_buf);
        return false;
    }

    // Center the image on 240x240 displays
    uint16_t img_w = info.width, img_h = info.height;
    int off_x = ((int)img_w > 240) ? 0 : (240 - (int)img_w) / 2;
    int off_y = ((int)img_h > 240) ? 0 : (240 - (int)img_h) / 2;

    int out_size = 0;
    jpeg_dec_get_outbuf_len(dec, &out_size);

    // For large images, decode scanline-by-scanline to avoid huge buffer
    // We'll decode the whole image if it fits in PSRAM, otherwise fall back
    uint8_t *outbuf = (uint8_t *)heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
    if (!outbuf) {
        jpeg_dec_close(dec);
        free(jpeg_buf);
        // Fallback: just show black
        eyeDisplayFillScreen(COL_BLACK);
        return true; // photo is saved, preview is optional
    }

    io.outbuf = outbuf;
    io.out_size = out_size;

    if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
        // Copy decoded RGB565 pixels centered into framebuffer
        eyeDisplayFillScreen(COL_BLACK);

        uint16_t *src = (uint16_t *)outbuf;
        int copy_w = (img_w < 240) ? img_w : 240;
        int copy_h = (img_h < 240) ? img_h : 240;
        for (int row = 0; row < copy_h; row++) {
            int dst_row = off_y + row;
            if (dst_row >= 240) break;
            for (int col = 0; col < copy_w; col++) {
                int dst_col = off_x + col;
                if (dst_col >= 240) break;
                eye_fb[dst_row * 240 + dst_col] = src[row * img_w + col];
            }
        }
        fbFlush();
    } else {
        fbFillScreen(COL_BLACK);
        fbFlush();
    }

    free(outbuf);
    jpeg_dec_close(dec);
    free(jpeg_buf);
    return true;
#else
    return false;
#endif
}

#endif // CAM_SD_ENABLED

// Command handlers
static void linkCmdStatus();
static void linkCmdEyes(const char *arg);
static void linkCmdAbort();
#if CAM_SD_ENABLED
static void linkCmdPhoto();
static void linkCmdRec(const char *arg);
static void linkCmdShow(const char *arg);
static void linkCmdList(const char *arg1, const char *arg2);
#endif

static void linkHandleLine(char *line) {
    char *verb = strtok(line, " ");
    if (!verb) return;
    char *arg1 = strtok(nullptr, " ");
    char *arg2 = strtok(nullptr, " ");

    if (strcmp(verb, "HELLO") == 0) {
        if (!arg1) { linkSend("ERR BAD_ARG"); return; }
        linkSend("OK CAM %s %d", CAM_FW_VERSION, CAM_PROTO_VERSION);
        return;
    }
    if (strcmp(verb, "PING") == 0)   { linkSend("OK PONG"); return; }
    if (strcmp(verb, "STATUS") == 0) { linkCmdStatus(); return; }
    if (strcmp(verb, "EYES") == 0)   { linkCmdEyes(arg1); return; }
    if (strcmp(verb, "PHOTO") == 0)  {
#if CAM_SD_ENABLED
        linkCmdPhoto();
#else
        linkSend("ERR SD");
#endif
        return;
    }
    if (strcmp(verb, "REC") == 0)    {
#if CAM_SD_ENABLED
        linkCmdRec(arg1);
#else
        linkSend("ERR SD");
#endif
        return;
    }
    if (strcmp(verb, "SHOW") == 0)   {
#if CAM_SD_ENABLED
        linkCmdShow(arg1);
#else
        linkSend("ERR SD");
#endif
        return;
    }
    if (strcmp(verb, "ABORT") == 0)  { linkCmdAbort(); return; }
    if (strcmp(verb, "LIST") == 0)   {
#if CAM_SD_ENABLED
        linkCmdList(arg1, arg2);
#else
        linkSend("ERR SD");
#endif
        return;
    }
    linkSend("ERR UNKNOWN_CMD");
}

static void linkCmdStatus() {
    // Get IP string
    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    linkSend("OK sd=%d busy=%d rec=%d expr=%s ip=%s rssi=%d uptime=%lld",
             sd_ready ? 1 : 0,
             link_state != LINK_IDLE ? 1 : 0,
             recording ? 1 : 0,
             eyeDisplayExpressionName(),
             ip_str,
             0, // RSSI (simplified)
             (long long)(esp_timer_get_time() / 1000000));
}

static void linkCmdEyes(const char *arg) {
    if (!arg || !eyeDisplaySetByName(arg)) { linkSend("ERR BAD_ARG"); return; }
    if (link_state == LINK_PHOTO_PREVIEW || link_state == LINK_SHOW) {
        link_state = LINK_IDLE;
        eyeDisplayResume();
    } else if (link_state != LINK_IDLE) {
        link_saved_expr = eyeDisplayGetExpression();
    }
    linkSend("OK EYES %s", arg);
}

static void linkCmdAbort() {
    if (link_state != LINK_IDLE) linkAbortFlow();
    linkSend("OK ABORT");
}

#if CAM_SD_ENABLED
static void linkCmdPhoto() {
    if (!sd_ready) { linkSend("ERR SD"); return; }
    if (recording || link_state != LINK_IDLE) { linkSend("ERR BUSY"); return; }
    link_saved_expr = eyeDisplayGetExpression();
    eyeDisplaySetExpression(EYE_NEUTRAL);
    eyeDisplayResume();
    link_state = LINK_PHOTO_PREPARE;
    link_state_at = esp_timer_get_time();
}

static void linkCmdRec(const char *arg) {
    if (!sd_ready) { linkSend("ERR SD"); return; }
    if (!arg) { linkSend("ERR BAD_ARG"); return; }
    if (strcmp(arg, "START") == 0) {
        if (link_state != LINK_IDLE) { linkSend("ERR BUSY"); return; }
        if (recording) { linkSend("ERR STATE"); return; }
        char path[40];
        if (!RecordStart(path, sizeof(path))) { linkSend("ERR IO"); return; }
        linkSend("OK REC ON %s", path);
        return;
    }
    if (strcmp(arg, "STOP") == 0) {
        if (!recording) { linkSend("ERR STATE"); return; }
        int32_t frames = RecordStop();
        if (frames < 0) { linkSend("ERR STATE"); return; }
        linkSend("OK REC OFF %s %ld", record_path, (long)frames);
        return;
    }
    linkSend("ERR BAD_ARG");
}

static void linkCmdShow(const char *arg) {
    if (!sd_ready) { linkSend("ERR SD"); return; }
    if (!arg) { linkSend("ERR BAD_ARG"); return; }
    if (link_state != LINK_IDLE) { linkSend("ERR BUSY"); return; }

    char path[64];
    if (strcmp(arg, "LATEST") == 0) {
        if (!LinkLatestPhoto(path, sizeof(path))) { linkSend("ERR NOENT"); return; }
    } else {
        if (!SanitizeSdPath(arg, path, sizeof(path)) ||
            strncmp(arg, "/photos/", 8) != 0) {
            linkSend("ERR BAD_ARG");
            return;
        }
        // Check file exists
        FILE *check = fopen(path, "rb");
        if (!check) { linkSend("ERR NOENT"); return; }
        fclose(check);
    }

    link_saved_expr = eyeDisplayGetExpression();
    if (!LinkShowJpegFile(path)) { linkSend("ERR IO"); return; }
    link_state = LINK_SHOW;
    link_state_at = esp_timer_get_time();
    linkSend("OK SHOW %s", path);
}

static void linkCmdList(const char *arg1, const char *arg2) {
    if (!sd_ready) { linkSend("ERR SD"); return; }
    if (!arg1 || strcmp(arg1, "photos") != 0) {
        linkSend("ERR BAD_ARG");
        return;
    }
    long max = -1;
    if (arg2 != nullptr) {
        max = atol(arg2);
        if (max < 0) { linkSend("ERR BAD_ARG"); return; }
    }

    // Count photos
    DIR *d = opendir("/sdcard/photos");
    long count = 0;
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_type == DT_REG) count++;
        }
        closedir(d);
    }

    long n = (max >= 0 && max < count) ? max : count;
    linkSend("OK BEGIN %ld", n);

    d = opendir("/sdcard/photos");
    long emitted = 0;
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr && emitted < n) {
            if (ent->d_type != DT_REG) continue;
            char full[48];
            snprintf(full, sizeof(full), "/sdcard/photos/%.31s", ent->d_name);
            struct stat st;
            unsigned fsize = 0;
            if (stat(full, &st) == 0) fsize = (unsigned)st.st_size;
            linkSend("F /photos/%s %u", ent->d_name, fsize);
            emitted++;
        }
        closedir(d);
    }
    linkSend("OK END");
}
#endif

static void camLinkInit() {
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 115200;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    ESP_ERROR_CHECK(uart_driver_install(CAM_LINK_UART, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(CAM_LINK_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CAM_LINK_UART, CAM_LINK_TX_PIN, CAM_LINK_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "EVT BOOT %d\n", CAM_PROTO_VERSION);
    uart_write_bytes(CAM_LINK_UART, buf, len);
    ESP_LOGI(TAG, "Cam link: UART%d 115200 RX=%d TX=%d, protocol v%d, fw %s",
             CAM_LINK_UART, CAM_LINK_RX_PIN, CAM_LINK_TX_PIN, CAM_PROTO_VERSION, CAM_FW_VERSION);
}

static void camLinkLoop() {
    // Read UART data
    uint8_t c;
    while (uart_read_bytes(CAM_LINK_UART, &c, 1, 0) > 0) {
        if (c == '\r') continue;
        if (c == '\n') {
            link_line[link_line_len] = 0;
            if (link_line_len > 0) linkHandleLine(link_line);
            link_line_len = 0;
        } else if (link_line_len < CAM_LINK_MAX_LINE - 1) {
            link_line[link_line_len++] = (char)c;
        } else {
            link_line_len = 0;
        }
    }

    // Photo flow state machine
    int64_t now = esp_timer_get_time();
    switch (link_state) {
        case LINK_PHOTO_PREPARE:
            if (now - link_state_at >= PHOTO_PREPARE_MS * 1000) {
                link_state = LINK_PHOTO_COUNTDOWN;
                link_state_at = now;
                link_countdown_digit = -1;
            }
            break;
        case LINK_PHOTO_COUNTDOWN: {
            int digit = 3 - (int)((now - link_state_at) / 1000000);
            if (digit < 1) {
#if CAM_SD_ENABLED
                linkPhotoCapture();
#else
                linkAbortFlow();
#endif
            } else if (digit != link_countdown_digit) {
                link_countdown_digit = digit;
                eyeDisplayShowNumber(digit);
            }
            break;
        }
        case LINK_PHOTO_PREVIEW:
        case LINK_SHOW:
            if (now - link_state_at >= LINK_PREVIEW_MS * 1000) linkAbortFlow();
            break;
        default:
            break;
    }
}

// ====================================================================
// HTTP Handlers
// ====================================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
    camera_fb_t *fb = nullptr;
    char partBuf[64];

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) continue;

        size_t hlen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, fb->len);
        esp_err_t res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, partBuf, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return ESP_OK;
}

static esp_err_t SendJson(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

static bool QueryParam(httpd_req_t *req, const char *key, char *out, size_t out_size) {
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 128) return false;
    char query[129];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    return httpd_query_key_value(query, key, out, out_size) == ESP_OK;
}

static esp_err_t indexHandler(httpd_req_t *req) {
    char page[512];
    // Get IP
    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    snprintf(page, sizeof(page),
             "Wall-E camera module is running.\n"
             "MJPEG stream: http://%s/stream\n"
             "Set this as stream_url in the main controller's web panel\n"
             "Eye displays: on (expr=%s, /eyes?expr=neutral|sad|left|right)\n"
             "%s"
             "%s"
             "Endpoints: /capture, /record?action=start|stop, /files, /file?path=...\n",
             ip_str,
             eyeDisplayExpressionName(),
             sd_ready ? "SD card: ready\n" : "SD card: NOT available\n",
             recording ? "Recording: yes\n" : "Recording: no\n");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, page, strlen(page));
}

static esp_err_t EyesHandler(httpd_req_t *req) {
    char expr[16];
    if (QueryParam(req, "expr", expr, sizeof(expr))) {
        if (!eyeDisplaySetByName(expr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"status\":\"Error\",\"msg\":\"unknown expr '%s'\"}", expr);
            return SendJson(req, buf);
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"OK\",\"expr\":\"%s\"}", eyeDisplayExpressionName());
    return SendJson(req, buf);
}

#if CAM_SD_ENABLED

static esp_err_t CaptureHandler(httpd_req_t *req) {
    if (!sd_ready) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"SD card not available\"}");

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Frame grab failed\"}");

    char path[40];
    NextSdPath(path, sizeof(path), "/photos", "IMG", ".jpg");
    FILE *f = fopen(path, "wb");
    if (!f) { esp_camera_fb_return(fb); return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Cannot open file on SD\"}"); }
    size_t written = fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    esp_camera_fb_return(fb);

    const char *name = strrchr(path, '/') + 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"OK\",\"file\":\"%s\",\"size\":%u}", name, (unsigned)written);
    return SendJson(req, buf);
}

static esp_err_t RecordHandler(httpd_req_t *req) {
    if (!sd_ready) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"SD card not available\"}");

    char action[8];
    if (!QueryParam(req, "action", action, sizeof(action))) {
        return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Missing action=start|stop\"}");
    }

    if (strcmp(action, "start") == 0) {
        if (recording) {
            const char *name = strrchr(record_path, '/') + 1;
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"status\":\"OK\",\"msg\":\"already recording\",\"file\":\"%s\"}", name);
            return SendJson(req, buf);
        }
        char path[40];
        if (!RecordStart(path, sizeof(path))) {
            return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Cannot start record task\"}");
        }
        const char *name = strrchr(path, '/') + 1;
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"status\":\"OK\",\"msg\":\"recording\",\"file\":\"%s\"}", name);
        return SendJson(req, buf);
    }

    if (strcmp(action, "stop") == 0) {
        if (!recording) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"not recording\"}");
        RecordStop();
        const char *name = strrchr(record_path, '/') + 1;
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"status\":\"OK\",\"file\":\"%s\",\"frames\":%lu,\"duration_ms\":%lld}",
                 name, (unsigned long)record_frames,
                 (long long)((esp_timer_get_time() - record_start_us) / 1000));
        return SendJson(req, buf);
    }

    return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Unknown action\"}");
}

static void AppendDirJson(char *json, size_t *pos, size_t buf_size, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_type != DT_REG) continue;
        char full[80];
        snprintf(full, sizeof(full), "%.39s/%.31s", dir, ent->d_name);
        struct stat st;
        long fsize = 0;
        if (stat(full, &st) == 0) fsize = (long)st.st_size;
        *pos += snprintf(json + *pos, buf_size - *pos, "%s{\"path\":\"%s\",\"size\":%ld}",
                         *pos > 15 ? "," : "", full + 8, fsize); // +8 skips "/sdcard"
    }
    closedir(d);
}

static esp_err_t FilesHandler(httpd_req_t *req) {
    if (!sd_ready) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"SD card not available\"}");

    static char json[2048];
    size_t pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "{\"status\":\"OK\",\"files\":[");
    AppendDirJson(json, &pos, sizeof(json), "/sdcard/photos");
    AppendDirJson(json, &pos, sizeof(json), "/sdcard/videos");
    pos += snprintf(json + pos, sizeof(json) - pos, "]}");
    return SendJson(req, json);
}

static esp_err_t FileGetHandler(httpd_req_t *req) {
    if (!sd_ready) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"SD card not available\"}");

    char param[64], path[80];
    if (!QueryParam(req, "path", param, sizeof(param)) ||
        !SanitizeSdPath(param, path, sizeof(path))) {
        return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Bad path\"}");
    }

    FILE *f = fopen(path, "rb");
    if (!f) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"File not found\"}");

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Range support for video playback
    size_t range_start = 0, range_end = file_size - 1;
    bool is_range = false;
    char range_hdr[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        unsigned long s = 0, e = 0;
        if (sscanf(range_hdr, "bytes=%lu-%lu", &s, &e) >= 1 && s < file_size) {
            is_range = true;
            range_start = s;
            range_end = (e == 0 || e >= file_size) ? file_size - 1 : e;
            fseek(f, range_start, SEEK_SET);
        }
    }

    const char *base = strrchr(path, '/') + 1;
    bool video = strstr(path, "/videos/") != nullptr;
    httpd_resp_set_type(req, video ? "video/x-msvideo" : "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    if (is_range) {
        httpd_resp_set_status(req, "206 Partial Content");
        char cr[80];
        snprintf(cr, sizeof(cr), "bytes %u-%u/%u",
                 (unsigned)range_start, (unsigned)range_end, (unsigned)file_size);
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    uint8_t buf[1024];
    size_t pos = range_start;
    while (pos <= range_end) {
        size_t want = range_end - pos + 1;
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n <= 0) break;
        pos += n;
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t FileDeleteHandler(httpd_req_t *req) {
    if (!sd_ready) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"SD card not available\"}");

    char param[64], path[80];
    if (!QueryParam(req, "path", param, sizeof(param)) ||
        !SanitizeSdPath(param, path, sizeof(path))) {
        return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Bad path\"}");
    }

    if (remove(path) != 0) return SendJson(req, "{\"status\":\"Error\",\"msg\":\"Delete failed\"}");
    return SendJson(req, "{\"status\":\"OK\"}");
}

#endif // CAM_SD_ENABLED

// ====================================================================
// SD card init
// ====================================================================

#if CAM_SD_ENABLED
static bool initSd() {
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI_PIN;
    bus_cfg.miso_io_num = SD_MISO_PIN;
    bus_cfg.sclk_io_num = SD_SCK_PIN;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = (gpio_num_t)SD_CS_PIN;
    slot_cfg.host_id = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    sdmmc_card_t *card;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s (photo/video disabled)", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return false;
    }

    // Create directories
    struct stat st;
    if (stat("/sdcard/photos", &st) != 0) mkdir("/sdcard/photos", 0755);
    if (stat("/sdcard/videos", &st) != 0) mkdir("/sdcard/videos", 0755);

    ESP_LOGI(TAG, "SD card mounted, %llu MB total",
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024)));
    return true;
}
#endif

// ====================================================================
// Record task (FreeRTOS)
// ====================================================================

#if CAM_SD_ENABLED
static void recordTaskFunc(void *) {
    FILE *f = fopen(record_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Record: cannot open %s", record_path);
        record_abnormal = true;
    } else {
        uint16_t w, h;
        FrameDims(&w, &h);
        AviWriter avi;
        avi.Begin(f, w, h, REC_FPS);

        TickType_t interval = pdMS_TO_TICKS(1000 / REC_FPS);
        TickType_t next = xTaskGetTickCount();
        int grab_fail_streak = 0;
        while (!record_stop_request) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                grab_fail_streak = 0;
                if (!avi.AddFrame(fb->buf, fb->len)) {
                    record_abnormal = true;
                } else {
                    record_frames = record_frames + 1;
                }
                esp_camera_fb_return(fb);
            } else if (++grab_fail_streak >= 20) {
                record_abnormal = true;
            }
            if (record_abnormal) break;
            vTaskDelayUntil(&next, interval);
        }
        avi.Finalize();
        fclose(f);
        ESP_LOGI(TAG, "Record: %s finalized, %lu frames", record_path, (unsigned long)record_frames);
    }
    recording = false;
    record_task = nullptr;
    if (record_abnormal) camLinkNotifyRecDone(record_path, record_frames);
    vTaskDelete(nullptr);
}
#endif

// ====================================================================
// Camera init
// ====================================================================

static bool initCamera() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = CAM_FRAME_SIZE;
    config.jpeg_quality = CAM_JPEG_QUALITY;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 0;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return false;
    }
    return true;
}

// ====================================================================
// Wi-Fi
// ====================================================================

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifiEventHandler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool connectWiFi(const char *ssid, const char *password, int timeout_ms) {
    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_LOGI(TAG, "Connecting to Wi-Fi: %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                            pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// ====================================================================
// HTTP Server
// ====================================================================

static void startStreamServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start camera stream server");
        return;
    }

    httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, nullptr};
    httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, nullptr};
    httpd_uri_t eyesUri = {"/eyes", HTTP_GET, EyesHandler, nullptr};
    httpd_register_uri_handler(server, &indexUri);
    httpd_register_uri_handler(server, &streamUri);
    httpd_register_uri_handler(server, &eyesUri);

#if CAM_SD_ENABLED
    httpd_uri_t captureUri = {"/capture", HTTP_GET, CaptureHandler, nullptr};
    httpd_uri_t recordUri = {"/record", HTTP_GET, RecordHandler, nullptr};
    httpd_uri_t filesUri = {"/files", HTTP_GET, FilesHandler, nullptr};
    httpd_uri_t fileGetUri = {"/file", HTTP_GET, FileGetHandler, nullptr};
    httpd_uri_t fileDelUri = {"/file", HTTP_DELETE, FileDeleteHandler, nullptr};
    httpd_register_uri_handler(server, &captureUri);
    httpd_register_uri_handler(server, &recordUri);
    httpd_register_uri_handler(server, &filesUri);
    httpd_register_uri_handler(server, &fileGetUri);
    httpd_register_uri_handler(server, &fileDelUri);
#endif

    ESP_LOGI(TAG, "Camera stream server started on port 80");
}

// ====================================================================
// Main
// ====================================================================

extern "C" void app_main() {
    ESP_LOGI(TAG, "--- Wall-E Camera Module (ESP32-S3) v%s ---", CAM_FW_VERSION);

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // TCP/IP + Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Eyes first (life sign)
    eyeDisplayInit();

    // UART link
    camLinkInit();

    // Camera
    if (!initCamera()) {
        ESP_LOGE(TAG, "Camera init failed - only UART link and eyes active");
        while (true) { camLinkLoop(); vTaskDelay(pdMS_TO_TICKS(10)); }
    }

    // SD card
#if CAM_SD_ENABLED
    sd_ready = initSd();
#endif

    // Wi-Fi
    esp_wifi_start();
    bool connected = false;
    if (strlen(CAM_WIFI_SSID) > 0) {
        connected = connectWiFi(CAM_WIFI_SSID, CAM_WIFI_PASSWORD, 15000);
    }
    if (!connected) {
        connected = connectWiFi(WALLE_AP_SSID, WALLE_AP_PASSWORD, 15000);
    }
    if (!connected) {
        ESP_LOGW(TAG, "Wi-Fi connection failed - restarting in 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    startStreamServer();

    // Main loop
    while (true) {
        eyeDisplayLoop();
        camLinkLoop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
