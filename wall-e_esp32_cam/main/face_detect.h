/**
 * CAM-SIDE FACE / BODY DETECTION (ESP-WHO + HSV colour matching)
 *
 * @file    face_detect.h
 * @brief   Initialise ESP-WHO models, run detection on camera frames,
 *          manage LOCK/UNLOCK (person reference), format EVT TRACK output.
 */

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/// One-time initialisation: load ESP-WHO face + body detection models.
/// Returns 0 on success, negative on error.
int face_detect_init();

/// Run one detection cycle on a JPEG frame. Writes an EVT TRACK line to
/// UART1 (CAM_LINK_UART) when results are ready. Called at ~5 Hz.
///
/// @param jpeg_buf   JPEG frame data (from esp_camera_fb_get)
/// @param jpeg_len   length in bytes
void face_detect_process(const uint8_t* jpeg_buf, size_t jpeg_len);

/// Lock onto the person currently in frame: store face + torso colour
/// reference for body-detection fallback. Returns 0 on success.
int face_detect_lock();

/// Clear the locked person reference. Returns 0 on success.
int face_detect_unlock();

/// True while a person reference is active (LOCK was called successfully).
bool face_detect_is_locked();

#ifdef __cplusplus
}
#endif
