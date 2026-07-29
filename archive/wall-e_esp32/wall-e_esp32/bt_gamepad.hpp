/**
 * BLUETOOTH GAMEPAD (Bluepad32)
 *
 * @file    bt_gamepad.hpp
 * @brief   Connects a Bluetooth gamepad (PS4/PS5/Xbox/Switch/generic,
 *          the optional gamepad from the third-party BOM) directly to
 *          the ESP32-S3 via the Bluepad32 library. Replaces the
 *          Raspberry Pi gamepad path (web_interface/gamepad.py).
 *
 * The mapping replicates gamepad.py / main.js exactly:
 *   left stick       -> drive X/Y (deadzone 0.2, send on change)
 *   right stick      -> head rotation G / neck T+B (incremental)
 *   LT/RT (held)     -> lower arms L/R;  LB/RB (press) -> raise arms
 *   A/B/X/Y          -> eye expressions i/l/j/k
 *   Back/Share       -> toggle autonomous servo mode (M0/M1)
 *   L3               -> arms neutral (n);  R3 -> head neutral (G50, g)
 *   D-Pad left/right -> random sound / random animation
 * All commands are dispatched through evaluateCommand(), identical to
 * the serial and HTTP control paths.
 *
 * NOTE on Bluepad32 API: written against Bluepad32 v4 for Arduino
 * (ESP32-S3 support). If the installed library version reports unknown
 * methods/constants, adjust the marked lines to the Gamepad class of
 * that version (names occasionally change between releases).
 */

#ifndef BT_GAMEPAD_HPP
#define BT_GAMEPAD_HPP

/// Initialise Bluepad32 and start scanning for gamepads.
/// Honours BT_GAMEPAD_ENABLED from web_config.h. Call from setup().
void btGamepadInit();

/// Poll the gamepad and dispatch commands (20ms interval).
/// Call on every iteration of loop(); returns immediately when idle.
void btGamepadLoop();

/// @return true when a gamepad is currently connected
bool btGamepadIsConnected();

/// @return true when the gamepad module is enabled and running
bool btGamepadIsActive();

#endif /* BT_GAMEPAD_HPP */
