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
 *    bundled esp32-camera library is used) plus the Arduino_GFX
 *    library (moononournation, for the eye displays) and the JPEGDEC
 *    library (bitbank2, for the eye-display photo preview).
 * 4. Upload, open the serial monitor (115200) and note the IP address.
 * 5. Enter that address as stream_url in wall-e_esp32/data/index.html
 *    (e.g. "http://192.168.4.3/stream") and re-upload LittleFS data.
 *
 * Endpoints:
 *   /        -> plain text status page (module name + stream URL + SD status)
 *   /stream  -> MJPEG stream (point the web interface here)
 *   /eyes    -> eye expression: /eyes?expr=neutral|sad|left|right
 *   /capture -> take a photo, save JPEG to SD (/photos/IMG_nnnn.jpg)
 *   /record?action=start|stop -> record MJPEG video as AVI (/videos/VID_nnnn.avi)
 *   /files   -> JSON list of photos/videos on the SD card
 *   /file?path=... -> download (GET) or delete (DELETE) a file
 *
 * UART link to the main controller (CAM_PROTOCOL v1, CAM side -
 * docs/CAM_PROTOCOL.md, implemented in cam_link.h):
 *   Serial1, 115200 8N1, RX=GPIO14 / TX=GPIO21 (pins UNVERIFIED, see
 *   NEW_HARDWARE_MIGRATION.md Step 4.5). Line-based text commands:
 *   HELLO / PING / STATUS / EYES / PHOTO (3-2-1 countdown on the eye
 *   displays + 5s local JPEG preview) / REC START|STOP / SHOW / ABORT
 *   / LIST photos. All HTTP endpoints above remain available as the
 *   fallback channel.
 *
 * Eye displays (third-party kit, CONFIRMED wiring 2026-07-31):
 *   Two round 1.28" 240x240 displays (driver assumed GC9A01) on a shared
 *   SPI bus: SCK=42, MOSI=45, DC=41, RST=46; per-display CS ("L/R" pin):
 *   left=2, right=0. See eye_display.h.
 *
 * SD card (pins follow the XIAO ESP32-S3 Sense preset - UNVERIFIED for
 * the actual generic ESP32-S3-CAM module, see NEW_HARDWARE_MIGRATION.md
 * Step 4.5; SD_CS_PIN=2 conflicts with the left eye CS):
 *   CS=2, SCK=7, MISO=8, MOSI=9, FAT32 card. Without a working card the
 *   stream keeps working; the SD endpoints return an error.
 */

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "camera_pins.h"
#include <SPI.h>
#include <SD.h>
#include "avi_writer.h"
#include "eye_display.h"


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

// SD card (SPI mode). WARNING: these pins follow the XIAO ESP32-S3 Sense
// preset and are UNVERIFIED for the actual generic "ESP32-S3-CAM" module
// (docs/NEW_HARDWARE_MIGRATION.md Step 4.5). SD_CS_PIN=2 also CONFLICTS
// with the confirmed eye-display wiring (EYE_L_CS=2, eye_display.h) -
// when the real SD pins are known, SD CS must move off GPIO2. If the SD
// init disturbs the eyes meanwhile, set CAM_SD_ENABLED to 0.
#define CAM_SD_ENABLED     1    // set 0 to compile without SD support
#define SD_CS_PIN          2
#define SD_SCK_PIN         7
#define SD_MISO_PIN        8
#define SD_MOSI_PIN        9
#define REC_FPS            15   // recording frame rate (VGA; drop if frames are skipped)


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


// -------------------------------------------------------------------
/// SD card state (declared early: indexHandler reports SD/recording status)
// -------------------------------------------------------------------

#if CAM_SD_ENABLED
static bool sd_ready = false;
static volatile bool recording = false;
#endif


static esp_err_t indexHandler(httpd_req_t *req) {
	String page = "Wall-E camera module is running.\n";
	page += "MJPEG stream: http://" + WiFi.localIP().toString() + "/stream\n";
	page += "Set this as stream_url in wall-e_esp32/data/index.html\n";
#if EYE_DISPLAYS_ENABLED
	page += String("Eye displays: on (expr=") + eyeDisplayExpressionName() + ", /eyes?expr=neutral|sad|left|right)\n";
#else
	page += "Eye displays: disabled at compile time\n";
#endif
#if CAM_SD_ENABLED
	page += sd_ready ? "SD card: ready\n" : "SD card: NOT available (photo/video disabled)\n";
	page += recording ? "Recording: yes\n" : "Recording: no\n";
	page += "Endpoints: /capture, /record?action=start|stop, /files, /file?path=...\n";
#endif
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_send(req, page.c_str(), page.length());
}


// -------------------------------------------------------------------
/// Shared HTTP helpers
// -------------------------------------------------------------------

static esp_err_t SendJson(httpd_req_t *req, const String& json) {
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, json.c_str(), json.length());
}

/// Query parameter helper: /record?action=start -> "start"
static bool QueryParam(httpd_req_t *req, const char* key, char* out, size_t out_size) {
	size_t qlen = httpd_req_get_url_query_len(req);
	if (qlen == 0 || qlen > 128) return false;
	char query[129];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
	return httpd_query_key_value(query, key, out, out_size) == ESP_OK;
}


// -------------------------------------------------------------------
/// Eye display endpoint: /eyes?expr=neutral|sad|left|right
// -------------------------------------------------------------------
/// Called with no query it just reports the current expression.

static esp_err_t EyesHandler(httpd_req_t *req) {
#if EYE_DISPLAYS_ENABLED
	char expr[16];
	if (QueryParam(req, "expr", expr, sizeof(expr))) {
		if (!eyeDisplaySetByName(expr)) {
			return SendJson(req, String("{\"status\":\"Error\",\"msg\":\"unknown expr '") + expr + "' (neutral|sad|left|right)\"}");
		}
	}
	return SendJson(req, String("{\"status\":\"OK\",\"expr\":\"") + eyeDisplayExpressionName() + "\"}");
#else
	return SendJson(req, "{\"status\":\"Error\",\"msg\":\"eye displays disabled at compile time\"}");
#endif
}


// -------------------------------------------------------------------
/// SD card: photo capture, AVI recording, file browsing
// -------------------------------------------------------------------

#if CAM_SD_ENABLED

// Recording state (driven by /record, worker task below)
static volatile bool record_stop_request = false;
static volatile bool record_abnormal = false;   // task ended on its own (SD/write/frame-grab failure)
static TaskHandle_t record_task = nullptr;
static char record_path[40];
static uint32_t record_frames = 0;
static unsigned long record_start_ms = 0;

// Defined in cam_link.h (included further below): reports an abnormal
// recording stop to the main controller as "EVT REC_DONE <path> <frames>".
void camLinkNotifyRecDone(const char* path, uint32_t frames);


static esp_err_t SendSdError(httpd_req_t *req, const char* msg) {
	return SendJson(req, String("{\"status\":\"Error\",\"msg\":\"") + msg + "\"}");
}

/// First free path like /photos/IMG_0007.jpg (collision-proof after deletes)
static void NextSdPath(char* out, size_t size, const char* dir, const char* prefix, const char* ext) {
	for (int i = 1; i < 10000; i++) {
		snprintf(out, size, "%s/%s_%04d%s", dir, prefix, i, ext);
		if (!SD.exists(out)) return;
	}
}

/// Pixel dimensions of the configured frame size (compile-time, no sensor query)
static void FrameDims(uint16_t* w, uint16_t* h) {
	switch (CAM_FRAME_SIZE) {
		case FRAMESIZE_QVGA: *w = 320;  *h = 240;  break;
		case FRAMESIZE_SVGA: *w = 800;  *h = 600;  break;
		case FRAMESIZE_XGA:  *w = 1024; *h = 768;  break;
		case FRAMESIZE_UXGA: *w = 1600; *h = 1200; break;
		case FRAMESIZE_VGA:
		default:             *w = 640;  *h = 480;  break;
	}
}


static esp_err_t CaptureHandler(httpd_req_t *req) {
	if (!sd_ready) return SendSdError(req, "SD card not available");

	camera_fb_t *fb = esp_camera_fb_get();
	if (!fb) return SendSdError(req, "Frame grab failed");

	char path[40];
	NextSdPath(path, sizeof(path), "/photos", "IMG", ".jpg");
	File f = SD.open(path, FILE_WRITE);
	if (!f) {
		esp_camera_fb_return(fb);
		return SendSdError(req, "Cannot open file on SD");
	}
	size_t written = f.write(fb->buf, fb->len);
	f.close();
	esp_camera_fb_return(fb);

	const char* name = strrchr(path, '/') + 1;
	return SendJson(req, String("{\"status\":\"OK\",\"file\":\"") + name +
		"\",\"size\":" + written + "}");
}


static void RecordTask(void*) {
	File f = SD.open(record_path, FILE_WRITE);
	if (!f) {
		Serial.printf("Record: cannot open %s\n", record_path);
		record_abnormal = true;
	} else {
		uint16_t w, h;
		FrameDims(&w, &h);
		AviWriter avi;
		avi.Begin(f, w, h, REC_FPS);

		const TickType_t interval = pdMS_TO_TICKS(1000 / REC_FPS);
		TickType_t next = xTaskGetTickCount();
		int grab_fail_streak = 0;
		while (!record_stop_request) {
			camera_fb_t *fb = esp_camera_fb_get();
			if (fb) {
				grab_fail_streak = 0;
				if (!avi.AddFrame(fb->buf, fb->len)) {
					record_abnormal = true;   // SD write failure (e.g. card full)
				} else {
					record_frames++;
				}
				esp_camera_fb_return(fb);
			} else if (++grab_fail_streak >= 20) {
				record_abnormal = true;       // persistent frame-grab failure
			}
			if (record_abnormal) break;
			// If the frame grab overran the interval this returns immediately
			vTaskDelayUntil(&next, interval);
		}
		avi.Finalize();
		f.close();
		Serial.printf("Record: %s finalized, %lu frames\n", record_path, record_frames);
	}
	recording = false;
	record_task = nullptr;
	// Abnormal termination is reported to the main controller as
	// EVT REC_DONE; a normal REC STOP is not (its OK REC OFF response
	// already carries path + frames - docs/CAM_PROTOCOL.md section 5).
	if (record_abnormal) camLinkNotifyRecDone(record_path, record_frames);
	vTaskDelete(nullptr);
}


// -------------------------------------------------------------------
/// Recording start/stop shared by the HTTP /record endpoint and the
/// UART cam link (cam_link.h, REC command)
// -------------------------------------------------------------------

/// Start a recording to a fresh /videos/VID_nnnn.avi path.
/// @return false when already recording or the worker task failed.
static bool RecordStart(char* out_path, size_t out_size) {
	if (recording) return false;
	NextSdPath(record_path, sizeof(record_path), "/videos", "VID", ".avi");
	record_frames = 0;
	record_start_ms = millis();
	record_stop_request = false;
	record_abnormal = false;
	recording = true;
	if (xTaskCreate(RecordTask, "cam_record", 4096, nullptr, 5, &record_task) != pdPASS) {
		recording = false;
		return false;
	}
	strncpy(out_path, record_path, out_size);
	out_path[out_size - 1] = 0;
	return true;
}

/// Stop the running recording and wait for the AVI to be finalized.
/// @return frames written, or -1 when not recording.
static int32_t RecordStop() {
	if (!recording) return -1;
	record_stop_request = true;
	// Wait for the task to finalize the AVI (max ~3s)
	for (int i = 0; i < 300 && recording; i++) vTaskDelay(pdMS_TO_TICKS(10));
	return (int32_t)record_frames;
}

static esp_err_t RecordHandler(httpd_req_t *req) {
	if (!sd_ready) return SendSdError(req, "SD card not available");

	char action[8];
	if (!QueryParam(req, "action", action, sizeof(action))) {
		return SendSdError(req, "Missing action=start|stop");
	}

	if (strcmp(action, "start") == 0) {
		if (recording) {
			const char* name = strrchr(record_path, '/') + 1;
			return SendJson(req, String("{\"status\":\"OK\",\"msg\":\"already recording\",\"file\":\"") + name + "\"}");
		}
		char path[40];
		if (!RecordStart(path, sizeof(path))) {
			return SendSdError(req, "Cannot start record task");
		}
		const char* name = strrchr(path, '/') + 1;
		return SendJson(req, String("{\"status\":\"OK\",\"msg\":\"recording\",\"file\":\"") + name + "\"}");
	}

	if (strcmp(action, "stop") == 0) {
		if (!recording) return SendSdError(req, "not recording");
		RecordStop();
		const char* name = strrchr(record_path, '/') + 1;
		return SendJson(req, String("{\"status\":\"OK\",\"file\":\"") + name +
			"\",\"frames\":" + record_frames +
			",\"duration_ms\":" + (millis() - record_start_ms) + "}");
	}

	return SendSdError(req, "Unknown action, use start|stop");
}


static void AppendDirJson(String& json, const char* dir, bool* first) {
	File d = SD.open(dir);
	if (!d) return;
	for (File f = d.openNextFile(); f; f = d.openNextFile()) {
		if (f.isDirectory()) { f.close(); continue; }
		const char* base = strrchr(f.name(), '/');
		base = base ? base + 1 : f.name();
		if (!*first) json += ",";
		*first = false;
		json += String("{\"path\":\"") + dir + "/" + base + "\",\"size\":" + f.size() + "}";
		f.close();
	}
	d.close();
}

static esp_err_t FilesHandler(httpd_req_t *req) {
	if (!sd_ready) return SendSdError(req, "SD card not available");
	String json = "{\"status\":\"OK\",\"files\":[";
	bool first = true;
	AppendDirJson(json, "/photos", &first);
	AppendDirJson(json, "/videos", &first);
	json += "]}";
	return SendJson(req, json);
}


/// Sanitize ?path= : must live under /photos/ or /videos/, no ".."
static bool SanitizeSdPath(const char* in, char* out, size_t out_size) {
	if (strlen(in) > out_size - 1) return false;
	if (strstr(in, "..")) return false;
	if (strncmp(in, "/photos/", 8) != 0 && strncmp(in, "/videos/", 8) != 0) return false;
	strcpy(out, in);
	return true;
}

static esp_err_t FileGetHandler(httpd_req_t *req) {
	if (!sd_ready) return SendSdError(req, "SD card not available");
	char param[64], path[64];
	if (!QueryParam(req, "path", param, sizeof(param)) || !SanitizeSdPath(param, path, sizeof(path))) {
		return SendSdError(req, "Bad path");
	}
	File f = SD.open(path, FILE_READ);
	if (!f) return SendSdError(req, "File not found");

	// HTTP Range support ("Range: bytes=start-end", end optional) for
	// frame-exact video playback on the main controller
	size_t file_size = f.size();
	size_t range_start = 0, range_end = file_size - 1;
	bool is_range = false;
	char range_hdr[64];
	if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
		unsigned long s = 0, e = 0;
		if (sscanf(range_hdr, "bytes=%lu-%lu", &s, &e) >= 1 && s < file_size) {
			is_range = true;
			range_start = s;
			range_end = (e == 0 || e >= file_size) ? file_size - 1 : e;
			f.seek(range_start);
		}
	}

	const char* base = strrchr(path, '/') + 1;
	bool video = strstr(path, "/videos/") != nullptr;
	httpd_resp_set_type(req, video ? "video/x-msvideo" : "image/jpeg");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
	String disp = String("attachment; filename=\"") + base + "\"";
	httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
	if (is_range) {
		httpd_resp_set_status(req, "206 Partial Content");
		char cr[80];
		snprintf(cr, sizeof(cr), "bytes %u-%u/%u",
			(unsigned)range_start, (unsigned)range_end, (unsigned)file_size);
		httpd_resp_set_hdr(req, "Content-Range", cr);
	}

	uint8_t buf[1024];
	size_t pos = range_start;
	while (pos <= range_end) {
		size_t want = range_end - pos + 1;
		if (want > sizeof(buf)) want = sizeof(buf);
		int n = f.read(buf, want);
		if (n <= 0) break;
		pos += n;
		if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) break;
	}
	f.close();
	httpd_resp_send_chunk(req, nullptr, 0);
	return ESP_OK;
}

static esp_err_t FileDeleteHandler(httpd_req_t *req) {
	if (!sd_ready) return SendSdError(req, "SD card not available");
	char param[64], path[64];
	if (!QueryParam(req, "path", param, sizeof(param)) || !SanitizeSdPath(param, path, sizeof(path))) {
		return SendSdError(req, "Bad path");
	}
	if (!SD.remove(path)) return SendSdError(req, "Delete failed");
	return SendJson(req, "{\"status\":\"OK\"}");
}

static bool initSd() {
	SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
	if (!SD.begin(SD_CS_PIN, SPI, 40000000)) {
		Serial.println(F("SD card mount failed (photo/video disabled, stream still works)"));
		return false;
	}
	if (!SD.exists("/photos")) SD.mkdir("/photos");
	if (!SD.exists("/videos")) SD.mkdir("/videos");
	Serial.printf("SD card mounted, %llu MB total\n", SD.totalBytes() / (1024 * 1024));
	return true;
}

#endif // CAM_SD_ENABLED

// UART cam link to the main controller (CAM_PROTOCOL v1) - included
// here so it can reuse the SD / recording helpers defined above
// (sd_ready, NextSdPath, SanitizeSdPath, RecordStart/RecordStop, ...).
// See cam_link.h and docs/CAM_PROTOCOL.md.
#include "cam_link.h"


static void startStreamServer() {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = 80;
	config.max_uri_handlers = 10;

	httpd_handle_t server = nullptr;
	if (httpd_start(&server, &config) != ESP_OK) {
		Serial.println(F("Failed to start camera stream server"));
		return;
	}

	httpd_uri_t indexUri = {"/", HTTP_GET, indexHandler, nullptr};
	httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, nullptr};
	httpd_uri_t eyesUri = {"/eyes", HTTP_GET, EyesHandler, nullptr};
	httpd_register_uri_handler(server, &indexUri);
	httpd_register_uri_handler(server, &streamUri);
	httpd_register_uri_handler(server, &eyesUri);

#if CAM_SD_ENABLED
	httpd_uri_t captureUri = {"/capture", HTTP_GET, CaptureHandler, nullptr};
	httpd_uri_t recordUri = {"/record", HTTP_GET, RecordHandler, nullptr};
	httpd_uri_t filesUri = {"/files", HTTP_GET, FilesHandler, nullptr};
	httpd_uri_t fileGetUri = {"/file", HTTP_GET, FileGetHandler, nullptr};
	httpd_uri_t fileDelUri = {"/file", HTTP_DELETE, FileDeleteHandler, nullptr};
	httpd_register_uri_handler(server, &captureUri);
	httpd_register_uri_handler(server, &recordUri);
	httpd_register_uri_handler(server, &filesUri);
	httpd_register_uri_handler(server, &fileGetUri);
	httpd_register_uri_handler(server, &fileDelUri);
#endif

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

	// Eyes first: they are confirmed wiring and serve as a life sign
	// during board bring-up, even if camera init fails below.
	eyeDisplayInit();

	// UART link to the main controller - before initCamera() so the
	// link (with its EVT BOOT announcement) is up even if the camera
	// sensor fails to initialise.
	camLinkInit();

	if (!initCamera()) {
		// Without a camera there is nothing to do; halt here, but keep
		// the UART link responsive so the main controller can still
		// query status and drive the eye displays.
		while (true) { camLinkLoop(); delay(10); }
	}

#if CAM_SD_ENABLED
	sd_ready = initSd();
#endif

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
	// The HTTP server runs in its own task; the eyes only need periodic
	// blink updates here (each blink frame blocks ~10-30ms for the
	// full-screen redraw, same as the archived ESP32 firmware).
	eyeDisplayLoop();
	// UART cam link: command parsing + photo-flow state machine
	camLinkLoop();
	delay(10);
}
