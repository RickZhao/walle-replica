/**
 * WALL-E CAMERA VIEWER
 *
 * @file    walle_cam_viewer.h
 * @brief   Shows photos and replays AVI recordings from the ESP32-S3-CAM
 *          module's SD card on the GC9A01 eye display, via
 *          LcdDisplay::SetPreviewImage().
 *
 * The cam module (wall-e_esp32_cam firmware) serves JPEG photos and
 * MJPEG-in-AVI recordings over HTTP (/files, /file?path=..., with Range
 * support). This viewer downloads a frame over HTTP, decodes it with
 * esp_new_jpeg into an RGB565 LvglAllocatedImage and hands it to the
 * display. Playback runs on a one-shot worker task so callers (MCP
 * tools, web handlers) never block; a second start request while busy
 * is rejected, and StopPlayback() (BOOT button or "stop" action)
 * terminates video replay after the current frame.
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
