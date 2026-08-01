/**
 * WALL-E CAMERA VIEWER  --  DEPRECATED / NO DISPLAY OUTPUT
 *
 * @file    walle_cam_viewer.h
 * @brief   DEPRECATED. The GC9A01 eye displays are wired to the
 *          ESP32-S3-CAM module, not to this MCU (config.h:
 *          EYE_DISPLAY_ENABLED=0, NoDisplay), so this HTTP-pull viewer has
 *          no display output. Photo preview / replay moved to CAM-local
 *          playback driven over the UART link (CAM_PROTOCOL v1, §11):
 *          use WalleCamLink::ShowLatest() / Show() / Abort() instead.
 *          MCP tools and the web panel (/camview) no longer call this
 *          class; it is kept for reference only (its HTTP Range + JPEG
 *          decode helpers may still be useful for a future v2 READ).
 *
 * Original purpose: showed photos and replayed AVI recordings from the
 * ESP32-S3-CAM module's SD card on the GC9A01 eye display, via
 * LcdDisplay::SetPreviewImage() (HTTP /files + /file?path=... with Range
 * support, esp_new_jpeg decode into an RGB565 LvglAllocatedImage).
 */

#pragma once

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WalleCamViewer {
public:
    static WalleCamViewer& GetInstance();

    // All starters return an empty string on success, or an error message.
    // Success means the worker task started; decode/display errors after
    // that point are only logged.
    std::string ShowLatestPhoto();                  // newest /photos/*.jpg
    std::string ShowPhoto(const std::string& path); // e.g. "/photos/IMG_0001.jpg"
    std::string PlayLatestVideo();                  // newest /videos/*.avi

    // Requests the worker to stop after the current frame and restores the
    // eye animation. Safe to call from any task; no-op when idle.
    void StopPlayback();

    bool IsBusy() const { return busy_.load(); }

private:
    WalleCamViewer() = default;

    enum class Op { kPhoto, kVideo };

    std::string StartOp(Op op, const std::string& path, size_t file_size);
    static void WorkerEntry(void* arg);
    void WorkerMain();
    void RunPhoto();
    void RunVideo();

    Op op_ = Op::kPhoto;
    char op_path_[64] = {0};
    size_t op_file_size_ = 0;   // from /files listing (0 = unknown)
    TaskHandle_t worker_ = nullptr;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_requested_{false};
};
