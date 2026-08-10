/**
 * WALL-E FACE TRACKER — implementation
 *
 * @file    walle_face_tracker.cc
 * @brief   See walle_face_tracker.h
 */

#include "walle_face_tracker.h"
#include "walle_motion.h"
#include "config.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#define TAG "FaceTracker"

// ===================================================================
// Singleton
// ===================================================================

WalleFaceTracker& WalleFaceTracker::GetInstance() {
    static WalleFaceTracker instance;
    return instance;
}

// ===================================================================
// Lifecycle
// ===================================================================

void WalleFaceTracker::Init() {
    if (started_) return;

#if FACE_TRACK_ENABLED
    BaseType_t ret = xTaskCreate(ControlTaskEntry, "face_track", 4096,
                                 this, 4, &task_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create control task");
        return;
    }
    started_ = true;
    ESP_LOGI(TAG, "Face tracker started (PID loop %d ms)", FACE_TRACK_PERIOD_MS);
#endif
}

void WalleFaceTracker::StartFollow() {
    if (!started_) {
        ESP_LOGW(TAG, "Face tracker not started");
        return;
    }
    // The caller (MCP tool / web handler) already sent LOCK to CAM.
    // Just enter SEARCH — the control task picks up incoming events.
    EnterState(State::kSearch);
    ESP_LOGI(TAG, "StartFollow → SEARCH");
}

void WalleFaceTracker::StopFollow() {
    EnterState(State::kIdle);
    // Stop any in-progress motion
    WalleMotion::GetInstance().EvaluateCommand('X', 0);
    WalleMotion::GetInstance().EvaluateCommand('Y', 0);
    last_turn_ = 0;
    last_move_ = 0;
    ESP_LOGI(TAG, "StopFollow → IDLE");
}

// ===================================================================
// Event callback (RX-task context — keep it fast)
// ===================================================================

void WalleFaceTracker::OnTrackEvent(const WalleCamLink::TrackEvent& evt) {
    if (!started_) return;

    target_type_.store(static_cast<TargetType>(evt.type));
    target_x_.store(evt.x);
    target_y_.store(evt.y);
    target_w_.store(evt.w);
    target_h_.store(evt.h);
    target_conf_.store(evt.conf);
    last_event_us_.store(esp_timer_get_time());
}

bool WalleFaceTracker::GetTargetBox(int& x, int& y, int& w, int& h, int& conf) const {
    if (target_type_.load() == TargetType::kNone) return false;
    x = target_x_.load();
    y = target_y_.load();
    w = target_w_.load();
    h = target_h_.load();
    conf = target_conf_.load();
    return true;
}

// ===================================================================
// State helpers
// ===================================================================

void WalleFaceTracker::EnterState(State s) {
    state_.store(s);
    state_entered_us_ = esp_timer_get_time();
    lost_since_us_ = 0;
}

const char* WalleFaceTracker::StateName(State s) const {
    switch (s) {
        case State::kIdle:   return "IDLE";
        case State::kSearch: return "SEARCH";
        case State::kFollow: return "FOLLOW";
        case State::kLost:   return "LOST";
    }
    return "?";
}

// ===================================================================
// Control task (20Hz)
// ===================================================================

void WalleFaceTracker::ControlTaskEntry(void* arg) {
    auto* self = static_cast<WalleFaceTracker*>(arg);
    self->ControlLoop();
    vTaskDelete(nullptr);
}

void WalleFaceTracker::ControlLoop() {
    TickType_t period = pdMS_TO_TICKS(FACE_TRACK_PERIOD_MS);
    TickType_t next = xTaskGetTickCount();

    while (true) {
        State s = state_.load();
        TargetType tt = target_type_.load();
        int64_t now_us = esp_timer_get_time();
        int64_t last_us = last_event_us_.load();

        switch (s) {
        case State::kIdle:
            // Nothing to do — wait for StartFollow()
            break;

        case State::kSearch: {
            // Got a face lock? → FOLLOW. Timeout? → IDLE.
            if (tt == TargetType::kFace) {
                EnterState(State::kFollow);
                ESP_LOGI(TAG, "SEARCH → FOLLOW (face found)");
            } else if (now_us - state_entered_us_ > FACE_TRACK_GIVEUP_MS * 1000LL) {
                ESP_LOGW(TAG, "SEARCH timeout — no face found");
                StopFollow();
            }
            break;
        }

        case State::kFollow: {
            if (tt == TargetType::kNone || (now_us - last_us) > FACE_TRACK_LOST_MS * 1000LL) {
                // Lost the target
                lost_since_us_ = now_us;
                EnterState(State::kLost);
                ESP_LOGI(TAG, "FOLLOW → LOST (no target for %lld ms)",
                         (now_us - last_us) / 1000);
                WalleMotion::GetInstance().EvaluateCommand('X', 0);
                WalleMotion::GetInstance().EvaluateCommand('Y', 0);
                last_turn_ = 0;
                last_move_ = 0;
            } else {
                PidStep();
            }
            break;
        }

        case State::kLost: {
            int64_t elapsed = now_us - lost_since_us_;
            if (tt != TargetType::kNone && (now_us - last_us) < FACE_TRACK_LOST_MS * 1000LL) {
                // Target reappeared
                EnterState(State::kFollow);
                ESP_LOGI(TAG, "LOST → FOLLOW (target recovered after %lld ms)",
                         elapsed / 1000);
            } else if (elapsed > FACE_TRACK_GIVEUP_MS * 1000LL) {
                ESP_LOGW(TAG, "LOST timeout (%lld ms) → IDLE", elapsed / 1000);
                StopFollow();
            }
            break;
        }
        } // switch

        vTaskDelayUntil(&next, period);
    }
}

// ===================================================================
// PID control
// ===================================================================

void WalleFaceTracker::PidStep() {
    int x = target_x_.load();
    int y = target_y_.load();
    int w = target_w_.load();
    int h = target_h_.load();
    int conf = target_conf_.load();
    TargetType tt = target_type_.load();

    if (w <= 0 || h <= 0) return;

    int face_cx = x + w / 2;
    int turn = 0, move = 0;

    if (ComputeMotorCommand(face_cx, w, h, turn, move)) {
        // Weight body-tracking confidence lower than face
        if (tt == TargetType::kBody) {
            turn = (int)(turn * FACE_TRACK_BODY_WEIGHT);
            move = (int)(move * FACE_TRACK_BODY_WEIGHT);
        }

        // Clamp speed
        int max_speed = FACE_TRACK_MAX_SPEED;
        turn = std::max(-max_speed, std::min(max_speed, turn));
        move = std::max(-max_speed, std::min(max_speed, move));

        // Apply dead zones — don't jitter
        if (std::abs(turn) < 5 && std::abs(move) < 5) {
            turn = 0;
            move = 0;
        }

        // Only issue commands when the value actually changed
        if (turn != last_turn_) {
            WalleMotion::GetInstance().EvaluateCommand('X', turn);
            last_turn_ = turn;
        }
        if (move != last_move_) {
            WalleMotion::GetInstance().EvaluateCommand('Y', move);
            last_move_ = move;
        }

        // Head servo — small horizontal offsets: turn head only
        int offset_pct = (face_cx - kFrameCenterX) * 100 / kFrameCenterX;
        int head_val = 50 + offset_pct / 2;  // center=50, range 0-100
        head_val = std::max(0, std::min(100, head_val));
        WalleMotion::GetInstance().EvaluateCommand('G', head_val);

        // Neck tilt — vertical tracking
        int face_cy = y + h / 2;
        int vert_offset = face_cy - kFrameH / 2;
        int neck_val = 50 - vert_offset / 5;  // center=50
        neck_val = std::max(0, std::min(100, neck_val));
        WalleMotion::GetInstance().EvaluateCommand('T', neck_val);
    }
}

bool WalleFaceTracker::ComputeMotorCommand(int face_cx, int target_w, int target_h,
                                            int& out_turn, int& out_move) {
    // Horizontal offset → turn
    int offset_x = face_cx - kFrameCenterX;
    int deadzone = kFrameW * FACE_TRACK_DEADZONE_PCT / 100;
    if (std::abs(offset_x) < deadzone) {
        out_turn = 0;
    } else {
        out_turn = (int)(offset_x * FACE_TRACK_TURN_GAIN * 100 / kFrameCenterX);
    }

    // Area → forward/backward
    float area_pct = (float)(target_w * target_h) * 100.0f / (kFrameW * kFrameH);
    if (area_pct > FACE_TRACK_TARGET_AREA * 1.5f) {
        // Too close → back up
        out_move = -(int)((area_pct - FACE_TRACK_TARGET_AREA) * FACE_TRACK_FWD_GAIN * 5);
    } else if (area_pct < FACE_TRACK_TARGET_AREA * 0.5f) {
        // Too far → move forward
        out_move = (int)((FACE_TRACK_TARGET_AREA - area_pct) * FACE_TRACK_FWD_GAIN * 5);
    } else {
        out_move = 0;
    }

    return true;
}
