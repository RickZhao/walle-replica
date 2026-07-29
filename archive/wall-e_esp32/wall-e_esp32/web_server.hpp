/**
 * WEB CONTROL INTERFACE
 *
 * @file    web_server.hpp
 * @brief   HTTP web interface for the ESP32-S3 Wall-E controller
 *
 * Replaces the Raspberry Pi Flask server (web_interface/app.py) so the
 * robot can be controlled standalone over Wi-Fi. The frontend in data/
 * is the same HTML/JS the Pi serves, so the browser experience is
 * identical. Control routes dispatch through evaluateCommand(), the
 * same entry point as the USB serial protocol, so behaviour is
 * identical on both control paths.
 *
 * Requires the libraries: ESPAsyncWebServer + AsyncTCP (ESP32Async org,
 * compatible with Arduino-ESP32 core 3.x) and LittleFS (bundled).
 */

#ifndef WEB_SERVER_HPP
#define WEB_SERVER_HPP

/// Initialise Wi-Fi (STA with AP fallback), mount LittleFS, register
/// all HTTP routes and start the server. Call once from setup().
void webServerInit();

/// Handle pending web-triggered actions (e.g. delayed restart).
/// Call on every iteration of loop(); returns immediately when idle.
void webServerLoop();

#endif /* WEB_SERVER_HPP */
