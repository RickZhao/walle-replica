/**
 * WALL-E FACE TRACKER
 *
 * @file    walle_face_tracker.h
 * @brief   Person-follow state machine + PID control loop.
 *
 * Receives EVT TRACK events (face / colour-matched body) from the CAM module
 * via WalleCamLink, runs a 20Hz PID control loop that dispatches motor (X/Y)
 * and head-servo (G/T/B) commands through WalleMotion::EvaluateCommand().
 *
 * State machine:
 *   IDLE → (follow_me) → SEARCH → (face found) → FOLLOW → (lost) → LOST → IDLE
 *
 * FOLLOW has two sub-modes:
 *   FOLLOW(face) — face visible, highest confidence
 *   FOLLOW(body) — face lost but colour-matched body still tracked (back view)
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "walle_cam_link.h"

class WalleFaceTracker {
public:
    enum class State {
        kIdle,
        kSearch,    // rotating head to find a face
        kFollow,    // actively tracking (face or body)
        kLost,      // target temporarily lost, extrapolating
    };

    enum class TargetType {
        kNone = 0,
        kFace = 1,   // EVT TRACK type 1
        kBody = 2,   // EVT TRACK type 2 (colour-matched back view)
    };

    static WalleFaceTracker& GetInstance();

    /// Start the 20Hz control task. Safe to call even if FACE_TRACK_ENABLED=0.
    void Init();

    /// IDLE → SEARCH: send LOCK to CAM, then start looking for faces.
    void StartFollow();

    /// Any state → IDLE: stop motors, clear tracking.
    void StopFollow();

    /// Called from WalleCamLink RX context when an EVT TRACK arrives.
    /// Updates the latest target bbox. Lightweight (no blocking).
    void OnTrackEvent(const WalleCamLink::TrackEvent& evt);

    /// Current state (for MCP / web panel).
    State GetState() const { return state_.load(); }

    /// Latest target type (face / body / none).
    TargetType GetTargetType() const { return target_type_.load(); }

    /// Latest bbox for web panel display. Returns true if we have a target.
    bool GetTargetBox(int& x, int& y, int& w, int& h, int& conf) const;

private:
    WalleFaceTracker() = default;
    WalleFaceTracker(const WalleFaceTracker&) = delete;
    WalleFaceTracker& operator=(const WalleFaceTracker&) = delete;

    static void ControlTaskEntry(void* arg);
    void ControlLoop();

    // PID step: convert bbox to motor/servo commands.
    void PidStep();

    // Compute motor X/Y values from horizontal offset and area ratio.
    // Returns true when a meaningful command was issued.
    bool ComputeMotorCommand(int face_cx, int target_w, int target_h,
                             int& out_turn, int& out_move);

    // State helpers
    void EnterState(State s);
    const char* StateName(State s) const;

    static constexpr int kFrameW = 320;   // QVGA (CAM detection resolution)
    static constexpr int kFrameH = 240;
    static constexpr int kFrameCenterX = kFrameW / 2;
    static constexpr float kTargetAreaIdeal = 12.0f;  // % of frame at ~2m

    // Shared state (written by RX task, read by control task)
    std::atomic<State> state_{State::kIdle};
    std::atomic<TargetType> target_type_{TargetType::kNone};
    std::atomic<int> target_x_{0}, target_y_{0}, target_w_{0}, target_h_{0}, target_conf_{0};
    std::atomic<int64_t> last_event_us_{0};    // last EVT TRACK timestamp

    // Control task internal state (only accessed from control task)
    int64_t lost_since_us_ = 0;
    int64_t state_entered_us_ = 0;
    int last_turn_ = 0;
    int last_move_ = 0;

    TaskHandle_t task_ = nullptr;
    bool started_ = false;
};
