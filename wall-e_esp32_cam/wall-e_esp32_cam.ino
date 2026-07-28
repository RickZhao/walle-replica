/**
 * WALL-E CAMERA MODULE (ESP32-S3-CAM)
 *
 * @file    wall-e_esp32_cam.ino
 * @brief   Standalone MJPEG streaming firmware for the second
 *          ESP32-S3-CAM module (third-party BOM item 13).
 *
 * Connection concept (confirmed): the camera board only takes power
 * from the robot's 5V supply; the data link is Wi-Fi. It first tries
 * to join your home Wi-Fi (CAM_WIFI_SSID below); if that fails it
 * joins the main controller's "WallE" access point instead. The
 * browser fetches the MJPEG stream directly from this module - the
 * main controller never proxies video.
 *
 * HOW TO USE:
 * 1. Fill in CAM_WIFI_SSID / CAM_WIFI_PASSWORD below (or leave empty
 *    to connect straight to the WallE AP). Do not commit credentials.
 * 2. Select your camera board in camera_pins.h.
 * 3. Install the esp32 board package (Arduino-ESP32 core 3.x - the
 *    bundled esp32-camera library is used, no extra install needed).
 * 4. Upload, open the serial monitor (115200) and note the IP address.
 * 5. Enter that address as stream_url in wall-e_esp32/data/index.html
 *    (e.g. "http://192.168.4.3/stream") and re-upload LittleFS data.
 *
 * Endpoints:
 *   /        -> plain text status page (module name + stream URL)
 *   /stream  -> MJPEG stream (point the web interface here)
 */

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "camera_pins.h"


/// Wi-Fi configuration
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
// Home Wi-Fi (station mode). Leave CAM_WIFI_SSID empty to skip.
// WARNING: do not commit real credentials to git.
#define CAM_WIFI_SSID      ""
#define CAM_WIFI_PASSWORD  ""

// Fallback: the main controller's access point
#define WALLE_AP_SSID      "WallE"
#define WALLE_AP_PASSWORD  "walle1234"

// Camera tuning
#define CAM_FRAME_SIZE     FRAMESIZE_VGA   // 640x480; drop to FRAMESIZE_SVGA/QVGA if flaky
#define CAM_JPEG_QUALITY   12              // 0-63, lower = better quality / larger frames


// -------------------------------------------------------------------
/// MJPEG streaming over esp_http_server
// -------------------------------------------------------------------

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";


static esp_err_t streamHandler(httpd_req_t *req) {

	camera_fb_t *fb = nullptr;
	char partBuf[64];

	if (httpd_resp_set_type(req, STREAM_CONTENT_TYPE) != ESP_OK) return ESP_FAIL;
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	while (true) {
		fb = esp_camera_fb_get();
		if (!fb) {
			// Frame grab failed - client probably disconnected or sensor busy
			continue;
		}

		size_t hlen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, fb->len);
		esp_err_t res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
		if (res == ESP_OK) res = httpd_resp_send_chunk(req, partBuf, hlen);
		if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

		esp_camera_fb_return(fb);

		if (res != ESP_OK) break;   // Client disconnected
	}
	return ESP_OK;
}


static esp_err_t indexHandler(httpd_req_t *req) {
	String page = "Wall-E camera module is running.\n";
	page += "MJPEG stream: http://" + WiFi.localIP().toString() + "/stream\n";
	page += "Set this as stream_url in wall-e_esp32/data/index.html\n";
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_send(req, page.c_str(), page.length());
}


static void startStreamServer() {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = 80;

	httpd_handle_t server = nullptr;
	if (httpd_start(&server, &config) != ESP_OK) {
		Serial.println(F("Failed to start camera stream server"));
		return;
	}

	httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, nullptr};
	httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, nullptr};
	httpd_register_uri_handler(server, &indexUri);
	httpd_register_uri_handler(server, &streamUri);

	Serial.println(F("Camera stream server started"));
}


// -------------------------------------------------------------------
/// Camera initialisation
// -------------------------------------------------------------------

static bool initCamera() {

	camera_config_t config;
	config.ledc_channel = LEDC_CHANNEL_0;
	config.ledc_timer = LEDC_TIMER_0;
	config.pin_d0 = Y2_GPIO_NUM;
	config.pin_d1 = Y3_GPIO_NUM;
	config.pin_d2 = Y4_GPIO_NUM;
	config.pin_d3 = Y5_GPIO_NUM;
	config.pin_d4 = Y6_GPIO_NUM;
	config.pin_d5 = Y7_GPIO_NUM;
	config.pin_d6 = Y8_GPIO_NUM;
	config.pin_d7 = Y9_GPIO_NUM;
	config.pin_xclk = XCLK_GPIO_NUM;
	config.pin_pclk = PCLK_GPIO_NUM;
	config.pin_vsync = VSYNC_GPIO_NUM;
	config.pin_href = HREF_GPIO_NUM;
	config.pin_sccb_sda = SIOD_GPIO_NUM;
	config.pin_sccb_scl = SIOC_GPIO_NUM;
	config.pin_pwdn = PWDN_GPIO_NUM;
	config.pin_reset = RESET_GPIO_NUM;
	config.xclk_freq_hz = 20000000;
	config.pixel_format = PIXFORMAT_JPEG;
	config.frame_size = CAM_FRAME_SIZE;
	config.jpeg_quality = CAM_JPEG_QUALITY;
	config.fb_count = 2;
	config.fb_location = CAMERA_FB_IN_PSRAM;
	config.grab_mode = CAMERA_GRAB_LATEST;
	config.sccb_i2c_port = 0;

	esp_err_t err = esp_camera_init(&config);
	if (err != ESP_OK) {
		Serial.printf("Camera init failed with error 0x%x\n", err);
		Serial.println(F("Check the pin preset in camera_pins.h matches your module"));
		return false;
	}
	return true;
}


// -------------------------------------------------------------------
/// Wi-Fi: home network first, WallE AP as fallback
// -------------------------------------------------------------------

static bool connectWiFi(const char *ssid, const char *password, int timeoutMs) {
	WiFi.begin(ssid, password);
	Serial.print(F("Connecting to Wi-Fi: "));
	Serial.println(ssid);

	unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - start < (unsigned long)timeoutMs) {
		delay(250);
	}
	return WiFi.status() == WL_CONNECTED;
}


// -------------------------------------------------------------------
/// Main
// -------------------------------------------------------------------

void setup() {
	Serial.begin(115200);
	Serial.println(F("--- Wall-E Camera Module (ESP32-S3) ---"));

	if (!initCamera()) {
		// Without a camera there is nothing to do; halt here
		while (true) delay(1000);
	}

	WiFi.mode(WIFI_STA);

	bool connected = false;
	if (strlen(CAM_WIFI_SSID) > 0) {
		connected = connectWiFi(CAM_WIFI_SSID, CAM_WIFI_PASSWORD, 15000);
	}
	if (!connected) {
		connected = connectWiFi(WALLE_AP_SSID, WALLE_AP_PASSWORD, 15000);
	}

	if (!connected) {
		Serial.println(F("Wi-Fi connection failed - restarting in 10s"));
		delay(10000);
		ESP.restart();
	}

	Serial.print(F("Wi-Fi connected, IP: "));
	Serial.println(WiFi.localIP());
	Serial.print(F("Stream URL: http://"));
	Serial.print(WiFi.localIP());
	Serial.println(F("/stream"));

	startStreamServer();
}

void loop() {
	// The HTTP server runs in its own task; nothing to do here
	delay(1000);
}
