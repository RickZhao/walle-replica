/**
 * STATUS DISPLAY (ST7789 1.3", 240x240)
 *
 * @file    status_display.hpp
 * @brief   Drives the 1.3" 7-pin colour screen from the third-party
 *          BOM as a status screen: battery level, Wi-Fi mode and IP,
 *          Bluetooth gamepad state and autonomous mode. Refreshes
 *          once per second.
 *
 * Requires the Arduino_GFX library (moononournation).
 * Enable/disable and pin mapping: web_config.h (DISPLAYS_ENABLED).
 */

#ifndef STATUS_DISPLAY_HPP
#define STATUS_DISPLAY_HPP

/// Initialise the ST7789 status screen. Call once from setup().
/// No-op when DISPLAYS_ENABLED is 0.
void statusDisplayInit();

/// Periodic refresh (1s interval). Call on every iteration of loop().
void statusDisplayLoop();

#endif /* STATUS_DISPLAY_HPP */
