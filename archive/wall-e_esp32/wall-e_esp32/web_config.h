/**
 * WEB CONTROL INTERFACE - CONFIGURATION
 *
 * @file    web_config.h
 * @brief   Wi-Fi and login settings for the ESP32-S3 web interface
 *
 * @warning Do NOT commit real Wi-Fi credentials to git.
 *          Leave WIFI_SSID empty to skip STA mode and run as a
 *          standalone access point instead.
 */

#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

// Station mode: connect to an existing Wi-Fi network.
// Leave WIFI_SSID empty ("") to skip STA and go straight to AP mode.
#define WIFI_SSID        ""
#define WIFI_PASSWORD    ""

// Access Point mode: the robot creates its own Wi-Fi network.
// Also used as fallback when STA connection fails (15s timeout).
// Password must be at least 8 characters (WPA2).
#define WIFI_AP_SSID     "WallE"
#define WIFI_AP_PASSWORD "walle1234"

// Web interface login password (same default as the Pi version)
#define WEB_LOGIN_PASSWORD "walle"

// HTTP port of the web interface
#define WEB_SERVER_PORT  80

// PCM5102 I2S audio output pins (audio chain: PCM5102 -> PAM8406 -> speakers)
// Avoid the GPIOs already used by motors/I2C/OE/battery (4-10, 15, 16, 17, 1)
#define I2S_BCLK_PIN     12   // PCM5102 BCK (bit clock)
#define I2S_LRC_PIN      13   // PCM5102 LRCK (word select)
#define I2S_DOUT_PIN     11   // PCM5102 DIN (data)

// Bluetooth gamepad via Bluepad32 (1 = enabled, 0 = disabled)
#define BT_GAMEPAD_ENABLED 1

// Displays (third-party BOM): 2x GC9A01 1.28" round (eyes) + 1x ST7789
// 1.3" 240x240 (status screen). All three share one SPI bus; each has
// its own CS/DC, RST and BL are tied together across the displays.
// Set to 0 when the displays are not wired up.
#define DISPLAYS_ENABLED  1
#define TFT_SPI_SCK       21   // shared SPI clock
#define TFT_SPI_MOSI      18   // shared SPI data (displays have no MISO)
#define TFT_RST           47   // shared reset line
#define TFT_BL            48   // shared backlight
#define EYE_L_CS          38   // left eye (GC9A01 #1)
#define EYE_L_DC          39
#define EYE_R_CS          40   // right eye (GC9A01 #2)
#define EYE_R_DC          41
#define STATUS_CS         42   // status screen (ST7789)
#define STATUS_DC         14

#endif /* WEB_CONFIG_H */
