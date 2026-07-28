/**
 * WEB CONTROL INTERFACE
 *
 * @file    web_server.cpp
 * @brief   HTTP web interface implementation (see web_server.hpp)
 *
 * Route compatibility with web_interface/app.py (Flask version):
 *   GET  /                 index.html (login required)
 *   GET  /login            login page
 *   POST /login_request    password check, sets session cookie
 *   POST /motor            joystick X/Y        -> evaluateCommand('X'/'Y')
 *   POST /settings         offsets/mode/etc.   -> evaluateCommand('O'/'S'/'M')
 *   POST /animate          animation clip      -> evaluateCommand('A')
 *   POST /servoControl     servo slider        -> evaluateCommand(prefix)
 *   POST /arduinoConnect   always "Connected" (control is internal)
 *   POST /arduinoStatus    battery level from the firmware variable
 *   GET|POST /gamepadStatus  disabled (Bluetooth gamepad is a later stage)
 *   POST /audio, /tts      report "not supported" (audio is a later stage)
 *   GET  /static/*         served from LittleFS
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "web_config.h"
#include "web_server.hpp"
#include "audio_player.hpp"
#include "bt_gamepad.hpp"

// Defined in wall-e_esp32.ino
extern void evaluateCommand(char prefix, int number);
extern int batteryLevel;


static AsyncWebServer server(WEB_SERVER_PORT);

// Pending delayed restart (millis() timestamp, 0 = none)
static unsigned long restartAt = 0;

// Camera streamer UI state (the stream itself lives on the separate
// ESP32-S3-CAM module; this only tracks the frontend toggle)
static bool streamerActive = false;


// -------------------------------------------------------------------
/// Helpers
// -------------------------------------------------------------------

static bool isAuthenticated(AsyncWebServerRequest *request) {
	if (!request->hasHeader("Cookie")) return false;
	return request->header("Cookie").indexOf("walle_auth=active") >= 0;
}

static void redirectTo(AsyncWebServerRequest *request, const char *url) {
	AsyncWebServerResponse *response = request->beginResponse(302);
	response->addHeader("Location", url);
	request->send(response);
}

// Returns true when the request may proceed; otherwise a redirect to
// the login page has already been sent (mirrors the Flask session check)
static bool requireAuth(AsyncWebServerRequest *request) {
	if (!isAuthenticated(request)) {
		redirectTo(request, "/login");
		return false;
	}
	return true;
}

static void sendJson(AsyncWebServerRequest *request, const String &json) {
	request->send(200, "application/json", json);
}

static void sendError(AsyncWebServerRequest *request, const char *msg) {
	String json = "{\"status\":\"Error\",\"msg\":\"";
	json += msg;
	json += "\"}";
	sendJson(request, json);
}

static bool hasParam(AsyncWebServerRequest *request, const char *name) {
	return request->hasParam(name, true);   // POST body parameters
}

static String paramValue(AsyncWebServerRequest *request, const char *name) {
	return request->getParam(name, true)->value();
}

// Template processor for login.html: %ERROR% expands to the alert
// block only after a failed login attempt
static String loginProcessor(const String &var) {
	if (var == "ERROR") return String();
	return String();
}

static String loginErrorProcessor(const String &var) {
	if (var == "ERROR") {
		return String(F("<div class=\"row set-row\" style=\"padding-top: 1em\">"
		                "<div class=\"col set-text\"></div>"
		                "<div class=\"col\"><div class=\"alert-col alert alert-danger\" style=\"max-width: 12em\">"
		                "<div id=\"alert-space\">Incorrect Password!</div>"
		                "</div></div></div>"));
	}
	return String();
}


// -------------------------------------------------------------------
/// Route registration
// -------------------------------------------------------------------

static void registerRoutes() {

	// -- Pages -------------------------------------------------------
	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		request->send(LittleFS, "/index.html", "text/html");
	});

	server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
		if (isAuthenticated(request)) {
			redirectTo(request, "/");
			return;
		}
		request->send(LittleFS, "/login.html", "text/html", false, loginProcessor);
	});

	server.on("/login_request", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (hasParam(request, "password") && paramValue(request, "password") == WEB_LOGIN_PASSWORD) {
			AsyncWebServerResponse *response = request->beginResponse(302);
			response->addHeader("Location", "/");
			response->addHeader("Set-Cookie", "walle_auth=active; Path=/");
			request->send(response);
		} else {
			request->send(LittleFS, "/login.html", "text/html", false, loginErrorProcessor);
		}
	});

	// -- Motor control (virtual joystick) ----------------------------
	server.on("/motor", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (hasParam(request, "stickX") && hasParam(request, "stickY")) {
			int xVal = int(paramValue(request, "stickX").toFloat() * 100);
			int yVal = int(paramValue(request, "stickY").toFloat() * 100);
			evaluateCommand('X', xVal);
			evaluateCommand('Y', yVal);
			sendJson(request, "{\"status\":\"OK\"}");
		} else {
			sendError(request, "Unable to read POST data");
		}
	});

	// -- Settings ----------------------------------------------------
	server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (!hasParam(request, "type") || !hasParam(request, "value")) {
			sendError(request, "Unable to read POST data");
			return;
		}

		String type = paramValue(request, "type");
		String value = paramValue(request, "value");

		if (type == "motorOff") {                    // Motor deadzone threshold
			evaluateCommand('O', value.toInt());
		} else if (type == "steerOff") {             // Steering offset/trim
			evaluateCommand('S', value.toInt());
		} else if (type == "animeMode") {            // Automatic/manual animation mode
			evaluateCommand('M', value.toInt());
		} else if (type == "volume") {               // Sound effects volume
			audioSetVolume(value.toInt());
		} else if (type == "streamer") {
			// The stream itself is served by the separate ESP32-S3-CAM
			// module; here we only toggle the frontend display state.
			streamerActive = !streamerActive;
			sendJson(request, streamerActive ? "{\"status\":\"OK\",\"streamer\":\"Active\"}"
			                                 : "{\"status\":\"OK\",\"streamer\":\"Offline\"}");
			return;
		} else if (type == "restart") {
			sendJson(request, "{\"status\":\"OK\"}");
			restartAt = millis() + 500;              // Restart after the response went out
			return;
		} else if (type == "shutdown") {
			sendError(request, "Shutdown not supported on ESP32");
			return;
		} else {
			sendError(request, "Unable to read POST data");
			return;
		}
		sendJson(request, "{\"status\":\"OK\"}");
	});

	// -- Animations --------------------------------------------------
	server.on("/animate", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (hasParam(request, "clip")) {
			evaluateCommand('A', paramValue(request, "clip").toInt());
			sendJson(request, "{\"status\":\"OK\"}");
		} else {
			sendError(request, "Unable to read POST data");
		}
	});

	// -- Manual servo control ----------------------------------------
	server.on("/servoControl", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (hasParam(request, "servo") && hasParam(request, "value")) {
			String servo = paramValue(request, "servo");
			if (servo.length() == 1) {
				evaluateCommand(servo.charAt(0), paramValue(request, "value").toInt());
				sendJson(request, "{\"status\":\"OK\"}");
			} else {
				sendError(request, "Invalid servo prefix");
			}
		} else {
			sendError(request, "Unable to read POST data");
		}
	});

	// -- Arduino connection (kept for frontend compatibility) --------
	// On the ESP32 the controller is internal, so it is always connected.
	server.on("/arduinoConnect", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (!hasParam(request, "action")) {
			sendError(request, "Unable to read [action] POST data");
			return;
		}
		String action = paramValue(request, "action");
		if (action == "updateList") {
			sendJson(request, "{\"status\":\"OK\",\"ports\":[\"Built-in (ESP32-S3)\"],\"portSelect\":0}");
		} else if (action == "reconnect") {
			sendJson(request, "{\"status\":\"OK\",\"arduino\":\"Connected\"}");
		} else {
			sendError(request, "Unable to read [action] POST data");
		}
	});

	// -- Arduino status (battery level) ------------------------------
	server.on("/arduinoStatus", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (hasParam(request, "type") && paramValue(request, "type") == "battery") {
			if (batteryLevel != -999) {
				sendJson(request, "{\"status\":\"OK\",\"battery\":" + String(batteryLevel) + "}");
			} else {
				sendJson(request, "{\"status\":\"Info\",\"msg\":\"No battery level available\"}");
			}
		} else {
			sendError(request, "Unable to read POST data");
		}
	});

	// -- Gamepad status (Bluetooth gamepad via Bluepad32) --------------
	server.on("/gamepadStatus", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request) {
		String json = "{\"status\":\"OK\",\"enabled\":";
		json += BT_GAMEPAD_ENABLED ? "true" : "false";
		json += ",\"active\":";
		json += btGamepadIsActive() ? "true" : "false";
		json += ",\"connected\":";
		json += btGamepadIsConnected() ? "true" : "false";
		json += "}";
		sendJson(request, json);
	});

	// -- Audio playback (PCM5102 I2S) ----------------------------------
	server.on("/audio", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		if (hasParam(request, "clip") && audioPlayClip(paramValue(request, "clip").c_str())) {
			sendJson(request, "{\"status\":\"OK\"}");
		} else {
			sendError(request, "Unable to play audio clip");
		}
	});

	// -- TTS (not ported - requires a cloud TTS service) ---------------
	server.on("/tts", HTTP_POST, [](AsyncWebServerRequest *request) {
		if (!requireAuth(request)) return;
		sendError(request, "Text-to-speech not supported in this build");
	});

	// -- Static frontend files ---------------------------------------
	server.serveStatic("/static/", LittleFS, "/static/").setCacheControl("max-age=86400");

	server.onNotFound([](AsyncWebServerRequest *request) {
		request->send(404, "text/plain", "Not found");
	});
}


// -------------------------------------------------------------------
/// Public interface
// -------------------------------------------------------------------

void webServerInit() {

	// Mount the filesystem holding the frontend files
	if (!LittleFS.begin(true)) {
		Serial.println(F("Warning: LittleFS mount failed - web interface unavailable"));
		return;
	}

	// Try station mode first (when credentials are configured)
	bool connected = false;
	if (strlen(WIFI_SSID) > 0) {
		Serial.print(F("Connecting to Wi-Fi: "));
		Serial.println(WIFI_SSID);
		WiFi.mode(WIFI_STA);
		WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

		unsigned long start = millis();
		while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
			delay(250);
		}
		connected = (WiFi.status() == WL_CONNECTED);
	}

	// Fall back to access point mode
	if (!connected) {
		Serial.println(F("Starting Wi-Fi access point"));
		WiFi.mode(WIFI_AP);
		WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
		Serial.print(F("AP SSID: ")); Serial.println(WIFI_AP_SSID);
		Serial.print(F("AP IP:   ")); Serial.println(WiFi.softAPIP());
	} else {
		Serial.print(F("Wi-Fi connected, IP: "));
		Serial.println(WiFi.localIP());
	}

	registerRoutes();
	server.begin();
	Serial.println(F("Web interface started"));
}

void webServerLoop() {
	if (restartAt != 0 && (long)(millis() - restartAt) >= 0) {
		ESP.restart();
	}
}
