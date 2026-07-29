/**
 * WALL-E STATUS DISPLAY (ST7789 1.3" 240x240)
 *
 * @file    walle_status_display.h
 * @brief   Secondary status screen on the shared SPI bus (CS=42, DC=14,
 *          SCK/MOSI/RST/BL shared with the GC9A01 eye display).
 *          Registered as a second LVGL display via esp_lvgl_port and
 *          refreshed once per second: battery, Wi-Fi, assistant state
 *          and autonomous mode.
 *
 * Port of the Arduino version's status_display.cpp (ST7789 status page).
 */

#pragma once

#include <esp_err.h>

class WalleStatusDisplay {
public:
    static WalleStatusDisplay& GetInstance();

    /// Initialise the ST7789 panel and the LVGL UI. Must be called AFTER
    /// the main display (the esp_lvgl_port task must already exist).
    esp_err_t Init();

private:
    WalleStatusDisplay() = default;

    void CreateUi();
    void Refresh();

    struct Impl;
    Impl* impl_ = nullptr;
};
