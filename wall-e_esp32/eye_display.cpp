/**
 * EYE DISPLAYS (2x GC9A01 1.28" round, 240x240)
 *
 * @file    eye_display.cpp
 * @brief   Implementation, see eye_display.hpp
 *
 * Eye rendering (per 240x240 round display, centred on 120,120):
 *   - light rounded-rect "lens" (the white of the eye)
 *   - dark iris disc, offset per expression (gaze direction)
 *   - small white highlight dot (makes it look alive)
 *   - eyelid: black bars closing from top and bottom (blink)
 *
 * Both displays sit on the shared SPI bus with individual CS/DC
 * (pins in web_config.h). Full-screen redraws are used throughout:
 * 240x240x2 bytes at typical SPI clocks lands in the 10-30ms range,
 * which is fast enough for blinks and expression changes.
 */

#include <Arduino_GFX_Library.h>
#include "web_config.h"
#include "eye_display.hpp"


#if DISPLAYS_ENABLED

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


static Arduino_DataBus *busL = nullptr;
static Arduino_DataBus *busR = nullptr;
static Arduino_GC9A01  *eyeL = nullptr;
static Arduino_GC9A01  *eyeR = nullptr;

static EyeExpression currentExpr = EYE_NEUTRAL;

// Blink state machine: lid goes 0 (open) -> 1 (closed) -> 0
static float lidClose = 0.0f;              // 0..1
static int   blinkPhase = 0;               // 0=idle, 1=closing, 2=opening
static unsigned long nextBlinkAt = 3000;
static unsigned long nextBlinkFrameAt = 0;


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
	drawEye(eyeL, currentExpr, lidClose);
	drawEye(eyeR, currentExpr, lidClose);
}

#endif /* DISPLAYS_ENABLED */


// -------------------------------------------------------------------
/// Public interface
// -------------------------------------------------------------------

void eyeDisplayInit() {
#if DISPLAYS_ENABLED
	// Both eyes share the SPI bus (SCK/MOSI) with individual CS/DC.
	// NOTE: if you add the status display too, all three use the same
	// host - Arduino_GFX re-selects the device per transaction via CS.
	busL = new Arduino_ESP32SPI(EYE_L_DC, EYE_L_CS, TFT_SPI_SCK, TFT_SPI_MOSI, GFX_NOT_DEFINED);
	busR = new Arduino_ESP32SPI(EYE_R_DC, EYE_R_CS, TFT_SPI_SCK, TFT_SPI_MOSI, GFX_NOT_DEFINED);

	eyeL = new Arduino_GC9A01(busL, TFT_RST, 0 /* rotation */, true /* IPS */);
	eyeR = new Arduino_GC9A01(busR, GFX_NOT_DEFINED /* RST shared */, 0, true);

	eyeL->begin();
	eyeR->begin();

	pinMode(TFT_BL, OUTPUT);
	digitalWrite(TFT_BL, HIGH);

	currentExpr = EYE_NEUTRAL;
	lidClose = 0.0f;
	redrawEyes();

	Serial.println(F("Eye displays started (2x GC9A01)"));
#endif
}


void eyeDisplayLoop() {
#if DISPLAYS_ENABLED
	unsigned long now = millis();

	// Start a new blink
	if (blinkPhase == 0 && (long)(now - nextBlinkAt) >= 0) {
		blinkPhase = 1;
		nextBlinkFrameAt = now;
	}

	// Animate the eyelid
	if (blinkPhase != 0 && (long)(now - nextBlinkFrameAt) >= 0) {
		nextBlinkFrameAt = now + BLINK_STEP_MS;

		if (blinkPhase == 1) {
			lidClose += 1.0f / BLINK_STEPS;
			if (lidClose >= 1.0f) {
				lidClose = 1.0f;
				blinkPhase = 2;
			}
		} else {
			lidClose -= 1.0f / BLINK_STEPS;
			if (lidClose <= 0.0f) {
				lidClose = 0.0f;
				blinkPhase = 0;
				nextBlinkAt = now + random(BLINK_MIN_MS, BLINK_MAX_MS);
			}
		}
		redrawEyes();
	}
#endif
}


void eyeDisplaySetExpression(EyeExpression expression) {
#if DISPLAYS_ENABLED
	if (expression == currentExpr) return;
	currentExpr = expression;
	redrawEyes();
#endif
}
