/**
 * WALL-E WEB CONTROL PANEL
 *
 * @file    walle_web_server.h
 * @brief   HTTP control server (esp_http_server) exposing the same API
 *          contract as the Raspberry Pi Flask version (web_interface/app.py):
 *            GET  /              embedded control page
 *            POST /motor         form: stickX, stickY  (-1.0..1.0)
 *            POST /settings      form: type, value (motorOff/steerOff/animeMode/restart)
 *            POST /animate       form: clip (animation id)
 *            POST /servoControl  form: servo (G/T/B/E/U/L/R/I/J), value (0..100)
 *            POST /arduinoStatus form: type=battery
 *            *    /gamepadStatus  always reports no gamepad (IDF port has none yet)
 *
 * All commands dispatch through WalleMotion::EvaluateCommand(), identical
 * to the serial protocol (docs/SERIAL_PROTOCOL.md). No login - only use
 * on a trusted LAN.
 */

#pragma once

#include <esp_err.h>

esp_err_t WalleWebServerStart();
void WalleWebServerStop();
