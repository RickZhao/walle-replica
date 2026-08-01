/**
 * UART CAM LINK (CAM_PROTOCOL v1, CAM side)
 *
 * @file    cam_link.h
 * @brief   Camera-module side of the line-based UART control protocol
 *          between the xiaozhi main controller and this ESP32-S3-CAM
 *          module. Protocol spec (final): docs/CAM_PROTOCOL.md.
 *
 *          Runs on Serial1, 115200 8N1. Pins RX=GPIO14 / TX=GPIO21 are
 *          UNVERIFIED on the actual module - see
 *          docs/NEW_HARDWARE_MIGRATION.md Step 4.5 (note GPIO14 is Y6
 *          in the XIAO preset of camera_pins.h, so the final board
 *          pinout may require moving CAM_LINK_RX_PIN). USB Serial
 *          remains free for debug logs; ALL protocol output goes to
 *          Serial1 only.
 *
 *          Commands: HELLO / PING / STATUS / EYES / PHOTO /
 *          REC START|STOP / SHOW / ABORT / LIST photos. The PHOTO flow
 *          (prepare -> 3-2-1 countdown on the eye displays -> capture
 *          -> 5s local JPEG preview -> restore expression) is a
 *          non-blocking state machine ticked from camLinkLoop(), so
 *          the HTTP MJPEG stream keeps running undisturbed in its own
 *          task. All HTTP endpoints remain available as fallback
 *          (docs/CAM_PROTOCOL.md section 10).
 *
 *          JPEG decoding for the eye-display preview uses the JPEGDEC
 *          library (bitbank2 - install "JPEGDEC" via the Arduino
 *          library manager). The file streams straight off the SD card
 *          through JPEGDEC::open(File&, callback), so no full-file RAM
 *          buffer is needed (a VGA photo fits nowhere near the heap).
 *          Decoded MCU blocks are pushed to both eye displays via
 *          eyeDisplayDrawRgbBitmap().
 *
 *          Requires (all defined above the include point in the .ino):
 *          eye_display.h, esp_camera, SD / sd_ready / NextSdPath /
 *          SanitizeSdPath / recording / record_path / RecordStart /
 *          RecordStop.
 */

#ifndef CAM_LINK_H
#define CAM_LINK_H

#include <JPEGDEC.h>
#include <stdarg.h>

#define CAM_FW_VERSION     "1.1.0"
#define CAM_PROTO_VERSION  1

// UART pins (Serial1). UNVERIFIED - confirm in Step 4.5 before wiring.
#define CAM_LINK_RX_PIN    14
#define CAM_LINK_TX_PIN    21

#define CAM_LINK_MAX_LINE  256     // protocol lines are <= 256 bytes

// PHOTO flow timing (docs/CAM_PROTOCOL.md section 4)
#define PHOTO_PREPARE_MS   500     // PREPARE: neutral look, exposure settles
#define PHOTO_COUNTDOWN_MS 3000    // COUNTDOWN: 3 -> 2 -> 1, 1s each
#define LINK_PREVIEW_MS    5000    // PREVIEW / SHOW playback duration


enum CamLinkState {
	LINK_IDLE = 0,
	LINK_PHOTO_PREPARE,
	LINK_PHOTO_COUNTDOWN,
	LINK_PHOTO_PREVIEW,
	LINK_SHOW,
};

static CamLinkState linkState = LINK_IDLE;
static unsigned long linkStateAt = 0;
static int linkCountdownDigit = -1;
static EyeExpression linkSavedExpr = EYE_NEUTRAL;   // expression to restore after a flow
static char linkLine[CAM_LINK_MAX_LINE];
static size_t linkLineLen = 0;
static JPEGDEC linkJpeg;


// -------------------------------------------------------------------
/// Protocol output helpers - Serial1 ONLY (Serial is for debug logs)
// -------------------------------------------------------------------

static void linkSend(const char *fmt, ...) {
	char buf[160];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	Serial1.print(buf);
	Serial1.print('\n');   // lines end with \n (no \r), per spec section 2
}

/// Called from RecordTask in the .ino when a recording ends abnormally
/// (SD full / write failure / repeated frame-grab failure). A normal
/// REC STOP does NOT emit this event - its OK REC OFF response already
/// carries path + frames (docs/CAM_PROTOCOL.md section 5).
void camLinkNotifyRecDone(const char *path, uint32_t frames) {
	Serial1.printf("EVT REC_DONE %s %lu\n", path, (unsigned long)frames);
}


// -------------------------------------------------------------------
/// Shared state helpers
// -------------------------------------------------------------------

static bool linkSdReady() {
#if CAM_SD_ENABLED
	return sd_ready;
#else
	return false;
#endif
}

static bool linkRecording() {
#if CAM_SD_ENABLED
	return recording;
#else
	return false;
#endif
}

/// End any running flow and restore the saved expression on the eyes.
static void linkAbortFlow() {
	linkState = LINK_IDLE;
	eyeDisplaySetExpression(linkSavedExpr);
	eyeDisplayResume();
}


#if CAM_SD_ENABLED

// -------------------------------------------------------------------
/// JPEG decode to the eye displays (photo preview / SHOW)
// -------------------------------------------------------------------

/// JPEGDEC draw callback: push each decoded MCU block to both eye
/// displays. pDraw->x/y already include the offset passed to decode().
static int LinkJpegDraw(JPEGDRAW *pDraw) {
	if (pDraw->iWidthUsed == pDraw->iWidth) {
		eyeDisplayDrawRgbBitmap(pDraw->x, pDraw->y, pDraw->pPixels,
		                        pDraw->iWidth, pDraw->iHeight);
	} else {
		// Clipped edge block: pPixels rows are iWidth wide, so draw
		// row by row with the used width only
		for (int row = 0; row < pDraw->iHeight; row++) {
			eyeDisplayDrawRgbBitmap(pDraw->x, pDraw->y + row,
			                        pDraw->pPixels + row * pDraw->iWidth,
			                        pDraw->iWidthUsed, 1);
		}
	}
	return 1;   // 1 = keep decoding
}

/// Decode a JPEG file from SD, scaled down to fit 240x240 and centred
/// on both eye displays. JPEGDEC supports binary scales only (1/2/4/8);
/// VGA 640x480 therefore shows as 160x120 centred. Returns false on
/// open/decode failure.
static bool LinkShowJpegFile(const char *path) {
	File f = SD.open(path, FILE_READ);
	if (!f) return false;

	if (!linkJpeg.open(f, LinkJpegDraw)) { f.close(); return false; }
	// Default is already little-endian RGB565 (= host-order uint16
	// colour values on ESP32), which is what Arduino_GFX expects.
	linkJpeg.setPixelType(RGB565_LITTLE_ENDIAN);

	// Largest binary scale that still fits the round 240x240 screens
	int w = linkJpeg.getWidth();
	int h = linkJpeg.getHeight();
	int scale = 0;
	while (scale != JPEG_SCALE_EIGHTH && (w > 240 || h > 240)) {
		w >>= 1;
		h >>= 1;
		scale = (scale == 0) ? JPEG_SCALE_HALF : (scale << 1);
	}

	eyeDisplayFillScreen(BLACK);
	bool ok = linkJpeg.decode((240 - w) / 2, (240 - h) / 2, scale) == 1;
	linkJpeg.close();   // also closes the File (JPEGDEC FileClose)
	return ok;
}

/// Newest photo on the SD card (IMG_nnnn.jpg names are zero-padded, so
/// lexicographic max = newest). Returns false when there are none.
static bool LinkLatestPhoto(char *out, size_t size) {
	File d = SD.open("/photos");
	if (!d) return false;
	bool found = false;
	for (File f = d.openNextFile(); f; f = d.openNextFile()) {
		if (!f.isDirectory()) {
			const char *base = strrchr(f.name(), '/');
			base = base ? base + 1 : f.name();
			char full[48];
			snprintf(full, sizeof(full), "/photos/%s", base);
			if (!found || strcmp(full, out) > 0) {
				strncpy(out, full, size);
				out[size - 1] = 0;
			}
			found = true;
		}
		f.close();
	}
	d.close();
	return found;
}

#endif // CAM_SD_ENABLED


// -------------------------------------------------------------------
/// Command implementations (docs/CAM_PROTOCOL.md section 3)
// -------------------------------------------------------------------

static void linkCmdStatus() {
	String ip = WiFi.localIP().toString();
	linkSend("OK sd=%d busy=%d rec=%d expr=%s ip=%s rssi=%d uptime=%lu",
		linkSdReady() ? 1 : 0,
		linkState != LINK_IDLE ? 1 : 0,
		linkRecording() ? 1 : 0,
		eyeDisplayExpressionName(),
		ip.c_str(),
		(int)WiFi.RSSI(),
		(unsigned long)(millis() / 1000));
}

static void linkCmdEyes(const char *arg) {
	if (arg == nullptr || !eyeDisplaySetByName(arg)) {
		linkSend("ERR BAD_ARG");
		return;
	}
	if (linkState == LINK_PHOTO_PREVIEW || linkState == LINK_SHOW) {
		// EYES during playback interrupts it and restores the eyes
		// immediately (docs/CAM_PROTOCOL.md section 4).
		linkState = LINK_IDLE;
		eyeDisplayResume();
	} else if (linkState != LINK_IDLE) {
		// Mid prepare/countdown: the flow continues, but RESTORE must
		// return to the expression just requested.
		linkSavedExpr = eyeDisplayGetExpression();
	}
	linkSend("OK EYES %s", arg);
}

static void linkCmdAbort() {
	if (linkState != LINK_IDLE) linkAbortFlow();
	linkSend("OK ABORT");
}

#if CAM_SD_ENABLED

static void linkCmdPhoto() {
	if (!sd_ready) { linkSend("ERR SD"); return; }
	if (recording || linkState != LINK_IDLE) { linkSend("ERR BUSY"); return; }

	linkSavedExpr = eyeDisplayGetExpression();
	// PREPARE: look straight ahead while the exposure settles
	eyeDisplaySetExpression(EYE_NEUTRAL);
	eyeDisplayResume();
	linkState = LINK_PHOTO_PREPARE;
	linkStateAt = millis();
}

/// CAPTURE stage: grab one frame, store it, answer OK FILE at once,
/// then start the local preview. Runs synchronously from camLinkLoop()
/// (a single frame grab + SD write, a few hundred ms worst case).
static void linkPhotoCapture() {
	camera_fb_t *fb = esp_camera_fb_get();
	if (!fb) {
		linkSend("ERR IO");
		linkAbortFlow();
		return;
	}

	char path[40];
	NextSdPath(path, sizeof(path), "/photos", "IMG", ".jpg");
	File f = SD.open(path, FILE_WRITE);
	if (!f) {
		esp_camera_fb_return(fb);
		linkSend("ERR SD");
		linkAbortFlow();
		return;
	}
	size_t written = f.write(fb->buf, fb->len);
	f.close();
	esp_camera_fb_return(fb);

	if (written == 0) {
		linkSend("ERR SD");
		linkAbortFlow();
		return;
	}

	// Answer as soon as the file is safely on the SD card
	// (docs/CAM_PROTOCOL.md section 4); playback is CAM-side cleanup.
	linkSend("OK FILE %s", path);

	// PREVIEW: decode the photo onto the eye displays; if decoding
	// fails, skip straight to RESTORE.
	if (!LinkShowJpegFile(path)) {
		linkAbortFlow();
		return;
	}
	linkState = LINK_PHOTO_PREVIEW;
	linkStateAt = millis();
}

static void linkCmdRec(const char *arg) {
	if (!sd_ready) { linkSend("ERR SD"); return; }
	if (arg == nullptr) { linkSend("ERR BAD_ARG"); return; }

	if (strcmp(arg, "START") == 0) {
		if (linkState != LINK_IDLE) { linkSend("ERR BUSY"); return; }
		if (recording) { linkSend("ERR STATE"); return; }
		char path[40];
		if (!RecordStart(path, sizeof(path))) { linkSend("ERR IO"); return; }
		linkSend("OK REC ON %s", path);
		return;
	}
	if (strcmp(arg, "STOP") == 0) {
		if (!recording) { linkSend("ERR STATE"); return; }
		int32_t frames = RecordStop();
		if (frames < 0) { linkSend("ERR STATE"); return; }
		linkSend("OK REC OFF %s %ld", record_path, (long)frames);
		return;
	}
	linkSend("ERR BAD_ARG");
}

static void linkCmdShow(const char *arg) {
	if (!sd_ready) { linkSend("ERR SD"); return; }
	if (arg == nullptr) { linkSend("ERR BAD_ARG"); return; }
	if (linkState != LINK_IDLE) { linkSend("ERR BUSY"); return; }

	char path[64];
	if (strcmp(arg, "LATEST") == 0) {
		if (!LinkLatestPhoto(path, sizeof(path))) { linkSend("ERR NOENT"); return; }
	} else {
		// Photos only (video playback on the eyes is v2, spec section 9)
		if (!SanitizeSdPath(arg, path, sizeof(path)) ||
		    strncmp(path, "/photos/", 8) != 0) {
			linkSend("ERR BAD_ARG");
			return;
		}
		if (!SD.exists(path)) { linkSend("ERR NOENT"); return; }
	}

	linkSavedExpr = eyeDisplayGetExpression();
	if (!LinkShowJpegFile(path)) { linkSend("ERR IO"); return; }
	linkState = LINK_SHOW;
	linkStateAt = millis();
	linkSend("OK SHOW %s", path);
}

static void linkCmdList(const char *arg1, const char *arg2) {
	if (!sd_ready) { linkSend("ERR SD"); return; }
	if (arg1 == nullptr || strcmp(arg1, "photos") != 0) {
		linkSend("ERR BAD_ARG");
		return;
	}
	long max = -1;
	if (arg2 != nullptr) {
		max = atol(arg2);
		if (max < 0) { linkSend("ERR BAD_ARG"); return; }
	}

	// Pass 1: count photos (directory order, unsorted)
	File d = SD.open("/photos");
	long count = 0;
	if (d) {
		for (File f = d.openNextFile(); f; f = d.openNextFile()) {
			if (!f.isDirectory()) count++;
			f.close();
		}
		d.close();
	}
	long n = (max >= 0 && max < count) ? max : count;
	linkSend("OK BEGIN %ld", n);

	// Pass 2: emit the first n entries
	d = SD.open("/photos");
	long emitted = 0;
	if (d) {
		for (File f = d.openNextFile(); f && emitted < n; f = d.openNextFile()) {
			if (f.isDirectory()) { f.close(); continue; }
			const char *base = strrchr(f.name(), '/');
			base = base ? base + 1 : f.name();
			linkSend("F /photos/%s %u", base, (unsigned)f.size());
			emitted++;
			f.close();
		}
		d.close();
	}
	linkSend("OK END");
}

#else  // !CAM_SD_ENABLED - storage commands all report ERR SD

static void linkCmdPhoto() { linkSend("ERR SD"); }
static void linkCmdRec(const char*) { linkSend("ERR SD"); }
static void linkCmdShow(const char*) { linkSend("ERR SD"); }
static void linkCmdList(const char*, const char*) { linkSend("ERR SD"); }

#endif // CAM_SD_ENABLED


// -------------------------------------------------------------------
/// Line parser + command dispatch
// -------------------------------------------------------------------

static void linkHandleLine(char *line) {
	char *verb = strtok(line, " ");
	if (verb == nullptr) return;
	char *arg1 = strtok(nullptr, " ");
	char *arg2 = strtok(nullptr, " ");

	if (strcmp(verb, "HELLO") == 0) {
		if (arg1 == nullptr) { linkSend("ERR BAD_ARG"); return; }
		// Version negotiation: we speak protocol v1 only; the master
		// takes min(its version, ours).
		linkSend("OK CAM %s %d", CAM_FW_VERSION, CAM_PROTO_VERSION);
		return;
	}
	if (strcmp(verb, "PING") == 0)   { linkSend("OK PONG"); return; }
	if (strcmp(verb, "STATUS") == 0) { linkCmdStatus(); return; }
	if (strcmp(verb, "EYES") == 0)   { linkCmdEyes(arg1); return; }
	if (strcmp(verb, "PHOTO") == 0)  { linkCmdPhoto(); return; }
	if (strcmp(verb, "REC") == 0)    { linkCmdRec(arg1); return; }
	if (strcmp(verb, "SHOW") == 0)   { linkCmdShow(arg1); return; }
	if (strcmp(verb, "ABORT") == 0)  { linkCmdAbort(); return; }
	if (strcmp(verb, "LIST") == 0)   { linkCmdList(arg1, arg2); return; }

	linkSend("ERR UNKNOWN_CMD");
}


// -------------------------------------------------------------------
/// Public interface
// -------------------------------------------------------------------

/// Start Serial1 and announce ourselves to the main controller.
/// Call once from setup() - before initCamera(), so the link is up
/// even when the camera sensor fails to initialise.
void camLinkInit() {
	Serial1.begin(115200, SERIAL_8N1, CAM_LINK_RX_PIN, CAM_LINK_TX_PIN);
	Serial1.printf("EVT BOOT %d\n", CAM_PROTO_VERSION);
	Serial.printf("Cam link: Serial1 115200 RX=%d TX=%d, protocol v%d, fw %s\n",
		CAM_LINK_RX_PIN, CAM_LINK_TX_PIN, CAM_PROTO_VERSION, CAM_FW_VERSION);
}

/// Non-blocking line parser + photo-flow state machine.
/// Call on every iteration of loop().
void camLinkLoop() {
	// Assemble command lines (\n terminated, \r ignored, <= 256 bytes)
	while (Serial1.available() > 0) {
		char c = (char)Serial1.read();
		if (c == '\r') continue;
		if (c == '\n') {
			linkLine[linkLineLen] = 0;
			if (linkLineLen > 0) linkHandleLine(linkLine);
			linkLineLen = 0;
		} else if (linkLineLen < CAM_LINK_MAX_LINE - 1) {
			linkLine[linkLineLen++] = c;
		} else {
			linkLineLen = 0;   // overlong line: drop it
		}
	}

	// Advance the PHOTO / SHOW state machine (docs/CAM_PROTOCOL.md 4)
	unsigned long now = millis();
	switch (linkState) {
		case LINK_PHOTO_PREPARE:
			if (now - linkStateAt >= PHOTO_PREPARE_MS) {
				linkState = LINK_PHOTO_COUNTDOWN;
				linkStateAt = now;
				linkCountdownDigit = -1;
			}
			break;

		case LINK_PHOTO_COUNTDOWN: {
			int digit = 3 - (int)((now - linkStateAt) / 1000);
			if (digit < 1) {
#if CAM_SD_ENABLED
				linkPhotoCapture();   // -> LINK_PHOTO_PREVIEW or aborts
#else
				linkAbortFlow();
#endif
			} else if (digit != linkCountdownDigit) {
				linkCountdownDigit = digit;
				eyeDisplayShowNumber(digit);
			}
			break;
		}

		case LINK_PHOTO_PREVIEW:
		case LINK_SHOW:
			// RESTORE: playback timed out, return to the saved expression
			if (now - linkStateAt >= LINK_PREVIEW_MS) linkAbortFlow();
			break;

		default:
			break;
	}
}

#endif /* CAM_LINK_H */
