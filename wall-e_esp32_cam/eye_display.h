/**
 * EYE DISPLAYS (2x round 1.28" 240x240, driver assumed GC9A01)
 *
 * @file    eye_display.h
 * @brief   Drives the two round eye displays of the third-party Wall-E
 *          kit from the ESP32-S3-CAM module (the displays are wired to
 *          the CAM board, not the main MCU). Renders vector-drawn eyes
 *          (no bitmap assets): lens + iris + pupil + highlight + eyelid,
 *          with expressions and periodic blinking.
 *
 *          Rendering logic is reused from the archived ESP32 firmware
 *          (archive/wall-e_esp32/wall-e_esp32/eye_display.cpp).
 *
 * Wiring (CONFIRMED 2026-07-31, per user measurement):
 *   shared SPI bus: SCK=GPIO42, SDA(MOSI)=GPIO45, DC=GPIO41, RST=GPIO46
 *   per-display select (silkscreen "L/R", acts as CS): left=GPIO2, right=GPIO0
 *   VDD=3V3, GND=GND. No backlight pin on these modules.
 *
 * Caution: GPIO0 (boot mode), GPIO45 (VDD_SPI) and GPIO46 (ROM log) are
 * strapping pins. CS idles high and SCL idles low, which is compatible
 * with their boot-time defaults - verify on hardware if boot issues appear.
 *
 * Driver IC is assumed GC9A01 (most common for 1.28" 240x240 round
 * modules); if the screens stay black, try Arduino_ST7789 (with offsets)
 * or check the vendor datasheet. Requires the Arduino_GFX library
 * (moononournation), same as the archived ESP32 firmware.
 */

#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H

#include <Arduino_GFX_Library.h>

#define EYE_DISPLAYS_ENABLED  1    // set 0 to compile without eye displays

// Confirmed wiring (third-party kit, 2026-07-31)
#define EYE_SPI_SCK   42
#define EYE_SPI_MOSI  45
#define EYE_SPI_DC    41
#define EYE_SPI_RST   46
#define EYE_L_CS      2     // left display "L" pin (acts as CS)
#define EYE_R_CS      0     // right display "R" pin (acts as CS); strapping pin, CS idles high = normal boot

enum EyeExpression {
	EYE_NEUTRAL,
	EYE_SAD,
	EYE_TILT_LEFT,
	EYE_TILT_RIGHT,
};

#if EYE_DISPLAYS_ENABLED

// Eye geometry (display is 240x240)
#define EYE_CX       120
#define EYE_CY       120
#define LENS_W       170     // lens rounded-rect width
#define LENS_H       130     // lens rounded-rect height
#define LENS_RADIUS  40
#define IRIS_RADIUS  38
#define GAZE_DX      22      // iris offset for tilt expressions
#define GAZE_DY_SAD  18      // iris offset downwards for sad

// Colours (RGB565)
#define COL_LENS     0xE71C  // light grey lens
#define COL_IRIS     0x2965  // dark blue-grey iris
#define COL_PUPIL    0x1082  // near-black pupil
#define COL_HILIGHT  0xFFFF  // white highlight

// Blink timing
#define BLINK_MIN_MS 2500    // minimum pause between blinks
#define BLINK_MAX_MS 6000    // maximum pause between blinks
#define BLINK_STEP_MS 40     // frame interval while blinking
#define BLINK_STEPS   4      // frames to close + frames to open


static Arduino_DataBus *eyeBusL = nullptr;
static Arduino_DataBus *eyeBusR = nullptr;
static Arduino_GC9A01  *eyeGfxL = nullptr;
static Arduino_GC9A01  *eyeGfxR = nullptr;

static EyeExpression eyeExpr = EYE_NEUTRAL;

// Blink state machine: lid goes 0 (open) -> 1 (closed) -> 0
static float eyeLidClose = 0.0f;           // 0..1
static int   eyeBlinkPhase = 0;            // 0=idle, 1=closing, 2=opening
static unsigned long eyeNextBlinkAt = 3000;
static unsigned long eyeNextBlinkFrameAt = 0;

// Overlay mode (cam_link.h countdown digits / decoded photo preview):
// while active the blink animation is suspended so it cannot clobber
// the overlay pixels.
static bool eyeOverlayActive = false;


// -------------------------------------------------------------------
/// Draw a single eye
///
/// @param  gfx   display to draw on
/// @param  expr  current expression
/// @param  lid   eyelid position, 0 = fully open, 1 = fully closed
// -------------------------------------------------------------------

static void drawEye(Arduino_GFX *gfx, EyeExpression expr, float lid) {

	gfx->fillScreen(BLACK);

	// Lens (white of the eye)
	gfx->fillRoundRect(EYE_CX - LENS_W / 2, EYE_CY - LENS_H / 2,
	                   LENS_W, LENS_H, LENS_RADIUS, COL_LENS);

	// Gaze offset per expression
	int dx = 0, dy = 0;
	if (expr == EYE_TILT_LEFT)  dx = -GAZE_DX;
	if (expr == EYE_TILT_RIGHT) dx =  GAZE_DX;
	if (expr == EYE_SAD)        dy =  GAZE_DY_SAD;

	// Iris + pupil + highlight
	gfx->fillCircle(EYE_CX + dx, EYE_CY + dy, IRIS_RADIUS, COL_IRIS);
	gfx->fillCircle(EYE_CX + dx, EYE_CY + dy, IRIS_RADIUS / 2, COL_PUPIL);
	gfx->fillCircle(EYE_CX + dx - 10, EYE_CY + dy - 10, 7, COL_HILIGHT);

	// Sad expression: angled upper eyelid covers the outer top of the eye
	if (expr == EYE_SAD) {
		gfx->fillTriangle(EYE_CX - LENS_W / 2, EYE_CY - LENS_H / 2,
		                  EYE_CX + LENS_W / 2, EYE_CY - LENS_H / 2,
		                  EYE_CX + LENS_W / 2, EYE_CY - 20,
		                  BLACK);
	}

	// Eyelid (blink): black bars closing from top and bottom
	if (lid > 0.0f) {
		int bar = int((LENS_H / 2 + 10) * lid);
		gfx->fillRect(0, 0, 240, EYE_CY - LENS_H / 2 + bar, BLACK);
		gfx->fillRect(0, EYE_CY + LENS_H / 2 - bar, 240, 240 - (EYE_CY + LENS_H / 2 - bar), BLACK);
	}
}


static void redrawEyes() {
	drawEye(eyeGfxL, eyeExpr, eyeLidClose);
	drawEye(eyeGfxR, eyeExpr, eyeLidClose);
}


// -------------------------------------------------------------------
/// Large countdown digit (7-segment style, white on black, no font)
// -------------------------------------------------------------------

// Segment bitmap per digit, bit0=a(top) ... bit6=g(middle)
static const uint8_t DIGIT_SEGMENTS[10] = {
	0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void drawDigit(Arduino_GFX *gfx, int digit) {
	// Centred 90x150 box with 16px thick segments
	const int W = 90, H = 150, T = 16;
	const int X0 = EYE_CX - W / 2, Y0 = EYE_CY - H / 2;
	uint8_t seg = DIGIT_SEGMENTS[digit];

	gfx->fillScreen(BLACK);
	if (seg & 0x01) gfx->fillRect(X0,         Y0,                 W, T,     WHITE);  // a: top
	if (seg & 0x02) gfx->fillRect(X0 + W - T, Y0,                 T, H / 2, WHITE);  // b: upper right
	if (seg & 0x04) gfx->fillRect(X0 + W - T, Y0 + H / 2,         T, H / 2, WHITE);  // c: lower right
	if (seg & 0x08) gfx->fillRect(X0,         Y0 + H - T,         W, T,     WHITE);  // d: bottom
	if (seg & 0x10) gfx->fillRect(X0,         Y0 + H / 2,         T, H / 2, WHITE);  // e: lower left
	if (seg & 0x20) gfx->fillRect(X0,         Y0,                 T, H / 2, WHITE);  // f: upper left
	if (seg & 0x40) gfx->fillRect(X0,         Y0 + H / 2 - T / 2, W, T,     WHITE);  // g: middle
}

#endif /* EYE_DISPLAYS_ENABLED */


// -------------------------------------------------------------------
/// Public interface
// -------------------------------------------------------------------

/// Initialise both eye displays and draw the neutral eyes.
/// Call once from setup(). No-op when EYE_DISPLAYS_ENABLED is 0.
void eyeDisplayInit() {
#if EYE_DISPLAYS_ENABLED
	// Both displays share one SPI bus (SCK/MOSI/DC) with individual CS.
	// HSPI (SPI3 host) is used so the SD card's FSPI bus is untouched.
	eyeBusL = new Arduino_ESP32SPI(EYE_SPI_DC, EYE_L_CS, EYE_SPI_SCK, EYE_SPI_MOSI,
	                               GFX_NOT_DEFINED /* MISO */, HSPI);
	eyeBusR = new Arduino_ESP32SPI(EYE_SPI_DC, EYE_R_CS, EYE_SPI_SCK, EYE_SPI_MOSI,
	                               GFX_NOT_DEFINED /* MISO */, HSPI);

	eyeGfxL = new Arduino_GC9A01(eyeBusL, EYE_SPI_RST, 0 /* rotation */, true /* IPS */);
	eyeGfxR = new Arduino_GC9A01(eyeBusR, GFX_NOT_DEFINED /* RST shared */, 0, true);

	eyeGfxL->begin();
	eyeGfxR->begin();

	eyeExpr = EYE_NEUTRAL;
	eyeLidClose = 0.0f;
	redrawEyes();

	Serial.println(F("Eye displays started (2x GC9A01, shared SPI SCK=42 MOSI=45 DC=41 RST=46, CS L=2 R=0)"));
#endif
}

/// Blink animation and timing. Call on every iteration of loop().
void eyeDisplayLoop() {
#if EYE_DISPLAYS_ENABLED
	// Overlay content (cam_link.h countdown / photo preview) owns the
	// screens - suspend blinking so it cannot clobber the pixels.
	if (eyeOverlayActive) return;

	unsigned long now = millis();

	// Start a new blink
	if (eyeBlinkPhase == 0 && (long)(now - eyeNextBlinkAt) >= 0) {
		eyeBlinkPhase = 1;
		eyeNextBlinkFrameAt = now;
	}

	// Animate the eyelid
	if (eyeBlinkPhase != 0 && (long)(now - eyeNextBlinkFrameAt) >= 0) {
		eyeNextBlinkFrameAt = now + BLINK_STEP_MS;

		if (eyeBlinkPhase == 1) {
			eyeLidClose += 1.0f / BLINK_STEPS;
			if (eyeLidClose >= 1.0f) {
				eyeLidClose = 1.0f;
				eyeBlinkPhase = 2;
			}
		} else {
			eyeLidClose -= 1.0f / BLINK_STEPS;
			if (eyeLidClose <= 0.0f) {
				eyeLidClose = 0.0f;
				eyeBlinkPhase = 0;
				eyeNextBlinkAt = now + random(BLINK_MIN_MS, BLINK_MAX_MS);
			}
		}
		redrawEyes();
	}
#endif
}

/// Change the current expression (redraws both eyes).
void eyeDisplaySetExpression(EyeExpression expression) {
#if EYE_DISPLAYS_ENABLED
	if (expression == eyeExpr) return;
	eyeExpr = expression;
	redrawEyes();
#endif
}

/// Set the expression by name ("neutral" / "sad" / "left" / "right",
/// same vocabulary as the main controller's self.walle.eyes MCP tool).
/// @return true when the name was recognised.
bool eyeDisplaySetByName(const char *name) {
	if (strcmp(name, "neutral") == 0)    { eyeDisplaySetExpression(EYE_NEUTRAL);    return true; }
	if (strcmp(name, "sad") == 0)        { eyeDisplaySetExpression(EYE_SAD);        return true; }
	if (strcmp(name, "left") == 0)       { eyeDisplaySetExpression(EYE_TILT_LEFT);  return true; }
	if (strcmp(name, "right") == 0)      { eyeDisplaySetExpression(EYE_TILT_RIGHT); return true; }
	return false;
}

/// Current expression name (for status reporting).
const char *eyeDisplayExpressionName() {
#if EYE_DISPLAYS_ENABLED
	switch (eyeExpr) {
		case EYE_SAD:        return "sad";
		case EYE_TILT_LEFT:  return "left";
		case EYE_TILT_RIGHT: return "right";
		case EYE_NEUTRAL:
		default:             return "neutral";
	}
#else
	return "disabled";
#endif
}


// -------------------------------------------------------------------
/// Overlay drawing interface (used by cam_link.h: photo countdown
/// digits and decoded JPEG preview). Overlay functions suspend the
/// blink animation; eyeDisplayResume() leaves overlay mode.
// -------------------------------------------------------------------

/// Current expression (cam_link.h saves it before the photo flow).
EyeExpression eyeDisplayGetExpression() {
#if EYE_DISPLAYS_ENABLED
	return eyeExpr;
#else
	return EYE_NEUTRAL;
#endif
}

/// Fill both displays with one colour (enters overlay mode).
void eyeDisplayFillScreen(uint16_t color) {
#if EYE_DISPLAYS_ENABLED
	eyeOverlayActive = true;
	eyeGfxL->fillScreen(color);
	eyeGfxR->fillScreen(color);
#endif
}

/// Draw an RGB565 block at the same position on BOTH displays
/// (JPEGDEC decode callback entry from cam_link.h). Pixel values are
/// host-order RGB565 (Arduino_GFX handles the wire byte order).
void eyeDisplayDrawRgbBitmap(int x, int y, const uint16_t *pixels, int w, int h) {
#if EYE_DISPLAYS_ENABLED
	eyeOverlayActive = true;
	eyeGfxL->draw16bitRGBBitmap(x, y, pixels, w, h);
	eyeGfxR->draw16bitRGBBitmap(x, y, pixels, w, h);
#endif
}

/// Show a large centred white digit (0-9) on both displays, black
/// background - the photo countdown (enters overlay mode).
void eyeDisplayShowNumber(int digit) {
#if EYE_DISPLAYS_ENABLED
	if (digit < 0 || digit > 9) return;
	eyeOverlayActive = true;
	drawDigit(eyeGfxL, digit);
	drawDigit(eyeGfxR, digit);
#endif
}

/// Leave overlay mode and redraw the current expression on both eyes.
void eyeDisplayResume() {
#if EYE_DISPLAYS_ENABLED
	eyeOverlayActive = false;
	redrawEyes();
#endif
}

#endif /* EYE_DISPLAY_H */
