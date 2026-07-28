/**
 * STATUS DISPLAY (ST7789 1.3", 240x240)
 *
 * @file    status_display.cpp
 * @brief   Implementation, see status_display.hpp
 *
 * Layout (240x240, text size 2, 16px line pitch):
 *
 *   +----------------------+
 *   |   WALL-E  (title)    |
 *   |   AP 192.168.4.1     |   or "STA 192.168.1.42"
 *   |   Battery 87%        |   (green / orange / red)
 *   |   Gamepad ON/OFF     |
 *   |   Auto mode ON/OFF   |
 *   |   Up 01:23           |
 *   +----------------------+
 */

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "web_config.h"
#include "status_display.hpp"
#include "bt_gamepad.hpp"

// Defined in wall-e_esp32.ino
extern int batteryLevel;
extern bool autoMode;


#if DISPLAYS_ENABLED

#define STATUS_REFRESH_MS 1000

static Arduino_DataBus *statusBus = nullptr;
static Arduino_ST7789  *statusTft = nullptr;

static unsigned long nextRefreshAt = 0;


static void drawStatus() {

	statusTft->fillScreen(BLACK);
	statusTft->setTextColor(WHITE, BLACK);
	statusTft->setTextSize(2);

	// Title
	statusTft->setCursor(24, 20);
	statusTft->setTextColor(0xFD20 /* orange */, BLACK);
	statusTft->print(F("= WALL-E ="));

	// Wi-Fi mode + IP
	statusTft->setCursor(12, 60);
	statusTft->setTextColor(WHITE, BLACK);
	if (WiFi.getMode() == WIFI_AP) {
		statusTft->print(F("AP   "));
		statusTft->print(WiFi.softAPIP().toString());
	} else {
		statusTft->print(F("WiFi "));
		statusTft->print(WiFi.localIP().toString());
	}

	// Battery level (colour-coded; hidden when no reading)
	statusTft->setCursor(12, 92);
	if (batteryLevel != -999) {
		uint16_t col = (batteryLevel > 65) ? 0x07E0 /* green */
		             : (batteryLevel > 35) ? 0xFD20 /* orange */
		             : 0xF800 /* red */;
		statusTft->setTextColor(col, BLACK);
		statusTft->printf("Batt %d%%   ", batteryLevel);
	} else {
		statusTft->setTextColor(0x7BEF /* grey */, BLACK);
		statusTft->print(F("Batt --"));
	}

	// Gamepad
	statusTft->setCursor(12, 124);
	statusTft->setTextColor(btGamepadIsConnected() ? 0x07E0 : 0x7BEF, BLACK);
	statusTft->print(btGamepadIsConnected() ? F("Gamepad ON ") : F("Gamepad off"));

	// Autonomous servo mode
	statusTft->setCursor(12, 156);
	statusTft->setTextColor(autoMode ? 0x07E0 : 0x7BEF, BLACK);
	statusTft->print(autoMode ? F("Auto ON   ") : F("Auto off  "));

	// Uptime
	statusTft->setCursor(12, 200);
	statusTft->setTextColor(0x7BEF, BLACK);
	unsigned long s = millis() / 1000;
	statusTft->printf("Up %02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

#endif /* DISPLAYS_ENABLED */


// -------------------------------------------------------------------
/// Public interface
// -------------------------------------------------------------------

void statusDisplayInit() {
#if DISPLAYS_ENABLED
	// Shares the SPI bus (SCK/MOSI) with the eye displays
	statusBus = new Arduino_ESP32SPI(STATUS_DC, STATUS_CS, TFT_SPI_SCK, TFT_SPI_MOSI, GFX_NOT_DEFINED);

	// 240x240 ST7789 IPS; the 1.3" panels usually need a row offset of
	// 80 at rotation 0 - adjust the offsets if the image is shifted
	statusTft = new Arduino_ST7789(statusBus, GFX_NOT_DEFINED /* RST shared */,
	                               0 /* rotation */, true /* IPS */,
	                               240, 240, 0, 0, 80, 0);

	statusTft->begin();
	drawStatus();

	Serial.println(F("Status display started (ST7789)"));
#endif
}


void statusDisplayLoop() {
#if DISPLAYS_ENABLED
	unsigned long now = millis();
	if ((long)(now - nextRefreshAt) >= 0) {
		nextRefreshAt = now + STATUS_REFRESH_MS;
		drawStatus();
	}
#endif
}
