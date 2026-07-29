/**
 * WALL-E MOTION CORE (ESP-IDF port of wall-e_esp32.ino)
 *
 * @file    walle_motion.h
 * @brief   Servo dynamics (trapezoidal velocity profiles), TB6612 motor
 *          control with acceleration ramping, animation queue, autonomous
 *          mode, battery monitoring and the shared command dispatcher
 *          (evaluateCommand - same semantics as the Arduino serial
 *          protocol, see docs/SERIAL_PROTOCOL.md).
 *
 * All control paths (MCP tools, USB serial, future web server) dispatch
 * through EvaluateCommand(), identical to the Arduino firmware.
 *
 * Calibration values (preset), the servo->channel mapping and the
 * animation data are carried over unchanged from the Arduino version.
 */

#pragma once

#include <stdint.h>
#include <esp_err.h>

class WalleMotion {
public:
    static WalleMotion& GetInstance();

    /// Initialise servo PWM driver, motors, battery ADC and start the
    /// 10ms motion control task. Performs the servo soft-start.
    esp_err_t Init();

    /// Register the self.walle.* / self.battery.* MCP tools.
    /// Implemented in walle_mcp_tools.cc; call once at board init.
    void RegisterMcpTools();

    /// Shared command dispatcher (prefix + numeric argument).
    /// Thread-safe to call from any task.
    void EvaluateCommand(char prefix, int number);

    /// Drive for a limited time, then stop automatically.
    /// @param move   forward/reverse speed (-100..100)
    /// @param turn   left/right turn (-100..100)
    /// @param duration_ms  stop after this time; 0 = keep driving
    void StartTimedMove(int move, int turn, int duration_ms);

    /// Last measured battery percentage, -999 when unavailable.
    int battery_level() const;

    /// True while the autonomous random-movement mode is on.
    bool auto_mode() const;

private:
    WalleMotion() = default;
    WalleMotion(const WalleMotion&) = delete;
    WalleMotion& operator=(const WalleMotion&) = delete;

    static void MotionTaskEntry(void* arg);
    void MotionTask();

    void ManageAnimations();
    void ManageServos(float dt_ms);
    void ManageMotors(float dt_ms);
    void PlayAnimation(int animation_no);
    void SoftStart();
    void CheckBattery();
    void UpdateMoveTimer();
};
