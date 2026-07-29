/**
 * BLUETOOTH GAMEPAD (Bluepad32)
 *
 * @file    bt_gamepad.cpp
 * @brief   Implementation, see bt_gamepad.hpp. Mapping replicates
 *          web_interface/gamepad.py (multipliers, deadzone, neutral
 *          poses, neck segment mapping) with per-poll increments
 *          tuned for a 20ms poll cycle.
 */

#include <Bluepad32.h>
#include "web_config.h"         // BT_GAMEPAD_ENABLED
#include "bt_gamepad.hpp"
#include "audio_player.hpp"     // D-Pad random sound

// Defined in wall-e_esp32.ino
extern void evaluateCommand(char prefix, int number);


// -- Tuning constants (mirroring web_interface/config.py) ------------
#define GP_POLL_INTERVAL_MS 20       // GAMEPAD_POLL_INTERVAL = 0.02s
#define GP_DEADZONE         0.2f     // GAMEPAD_DEADZONE
#define GP_HEAD_STEP        1.5f     // per-poll increment at full deflection
#define GP_NECK_STEP        1.5f     // (head/neck: 0..100 / 0..200 range)
#define GP_ARM_STEP         1.2f     // LT/RT held: arm decrement per poll
#define GP_ARM_BUTTON_STEP  6        // LB/RB press: arm increment
#define GP_ANIMATION_COUNT  3        // animations.ino cases 0..2


#if BT_GAMEPAD_ENABLED

static ControllerPtr gamepad = nullptr;
static bool connected = false;
static bool autoMode = false;

// Runtime state (mirrors gamepad.py: moveHead/moveArms equivalents)
static float headRotation = 50.0f;   // 0..100
static float neck = 125.0f;          // 0..200 (segment-mapped to T/B)
static float armLeft = 50.0f;        // 0..100
static float armRight = 50.0f;       // 0..100

static int lastX = 0;
static int lastY = 0;

static unsigned long lastPoll = 0;

// Previous button states for edge detection
static uint16_t prevButtons = 0;
static uint8_t  prevDpad = 0;
static uint8_t  prevMisc = 0;


// -------------------------------------------------------------------
/// Connection callbacks
// -------------------------------------------------------------------

static void onConnected(ControllerPtr ctl) {
	if (gamepad == nullptr) {
		gamepad = ctl;
		connected = true;
		Serial.print(F("Gamepad connected: "));
		Serial.println(ctl->getModelName());
		// Zero the drive on connect, just in case
		lastX = lastY = 0;
		evaluateCommand('X', 0);
		evaluateCommand('Y', 0);
	}
}

static void onDisconnected(ControllerPtr ctl) {
	if (gamepad == ctl) {
		Serial.println(F("Gamepad disconnected"));
		gamepad = nullptr;
		connected = false;
		// Stop the robot when the gamepad drops out
		lastX = lastY = 0;
		evaluateCommand('X', 0);
		evaluateCommand('Y', 0);
	}
}


// -------------------------------------------------------------------
/// Helpers
// -------------------------------------------------------------------

static float applyDeadzone(float v) {
	return (v < GP_DEADZONE && v > -GP_DEADZONE) ? 0.0f : v;
}

static float clamp(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

// Neck position (0..200) -> T/B segment mapping (gamepad.py _send_neck)
static void sendNeck() {
	int n = int(neck);
	if (n < 100) {
		evaluateCommand('T', n);
		evaluateCommand('B', 0);
	} else if (n < 160) {
		evaluateCommand('T', 200 - n);
		evaluateCommand('B', n - 100);
	} else {
		evaluateCommand('T', n - 110);
		evaluateCommand('B', 60);
	}
}

static void playRandomAnimation() {
	evaluateCommand('A', int(random(GP_ANIMATION_COUNT)));
}

// Sound clip base names, mirroring data/static/sounds/
static const char *soundClips[] = {
	"Sound_Ohhh_1850", "Sound_Raspberry_1550", "Sound_Tada-long_10200",
	"Sound_Tada_1500", "Sound_Uh-Huh_550", "Sound_Whistle-1_1150",
	"Sound_Whistle-2_550", "Sound_Whoa_1300", "Sound_Wow_810",
	"Voice_Walle-1_1950", "Voice_Walle-2_3900", "Voice_Walle-3_1700",
};

static void playRandomSound() {
	audioPlayClip(soundClips[random(sizeof(soundClips) / sizeof(soundClips[0]))]);
}


// -------------------------------------------------------------------
/// Button edge handling (pressed = current & ~previous)
// -------------------------------------------------------------------

static void handleButtons(uint16_t pressed) {
	// Face buttons -> eye expressions (single-character commands)
	if (pressed & BUTTON_A) evaluateCommand('i', 0);        // Sad eyes
	if (pressed & BUTTON_B) evaluateCommand('l', 0);        // Head tilt right
	if (pressed & BUTTON_X) evaluateCommand('j', 0);        // Head tilt left
	if (pressed & BUTTON_Y) evaluateCommand('k', 0);        // Neutral eyes

	// Bumpers -> raise arms
	if (pressed & BUTTON_SHOULDER_L) {
		armLeft = clamp(armLeft + GP_ARM_BUTTON_STEP, 0, 100);
		evaluateCommand('L', int(armLeft));
	}
	if (pressed & BUTTON_SHOULDER_R) {
		armRight = clamp(armRight + GP_ARM_BUTTON_STEP, 0, 100);
		evaluateCommand('R', int(armRight));
	}

	// Stick clicks -> neutral poses
	if (pressed & BUTTON_THUMB_L) {                 // L3: arms neutral
		armLeft = armRight = 50.0f;
		evaluateCommand('n', 0);
	}
	if (pressed & BUTTON_THUMB_R) {                 // R3: head neutral
		headRotation = 50.0f;
		neck = 125.0f;
		evaluateCommand('G', 50);
		evaluateCommand('g', 0);
	}
}

static void handleMisc(uint8_t pressed) {
	// Back/Share button -> toggle autonomous servo mode.
	// NOTE: MISC_BUTTON_BACK is bit 0x02 of miscButtons() in Bluepad32 v4;
	// if your library version names it differently, adjust here.
	if (pressed & 0x02) {
		autoMode = !autoMode;
		evaluateCommand('M', autoMode ? 1 : 0);
	}
}

static void handleDpad(uint8_t pressed) {
	if (pressed & DPAD_LEFT)  playRandomSound();
	if (pressed & DPAD_RIGHT) playRandomAnimation();
}


// -------------------------------------------------------------------
/// Polling
// -------------------------------------------------------------------

static void pollGamepad() {

	// -- Left stick -> drive (send only on change) -------------------
	float lx = applyDeadzone(gamepad->axisX() / 512.0f);
	float ly = applyDeadzone(gamepad->axisY() / 512.0f);

	int x = int(clamp(lx * 100, -100, 100));
	int y = int(clamp(-ly * 100, -100, 100));

	if (x != lastX) {
		evaluateCommand('X', x);
		lastX = x;
	}
	if (y != lastY) {
		evaluateCommand('Y', y);
		lastY = y;
	}

	// -- Right stick -> head rotation / neck (incremental) -----------
	float rx = applyDeadzone(gamepad->axisRX() / 512.0f);
	float ry = applyDeadzone(gamepad->axisRY() / 512.0f);

	if (rx != 0.0f) {
		headRotation = clamp(headRotation + rx * GP_HEAD_STEP, 0, 100);
		evaluateCommand('G', int(headRotation));
	}
	if (ry != 0.0f) {
		neck = clamp(neck + ry * GP_NECK_STEP, 0, 200);
		sendNeck();
	}

	// -- Analog triggers (held) -> lower arms ------------------------
	// brake()/throttle() return 0..1023; treat l2()/r2() as the
	// digital fallback for gamepads without analog triggers.
	bool ltHeld = (gamepad->brake() > 200) || gamepad->l2();
	bool rtHeld = (gamepad->throttle() > 200) || gamepad->r2();

	if (ltHeld) {
		armLeft = clamp(armLeft - GP_ARM_STEP, 0, 100);
		evaluateCommand('L', int(armLeft));
	}
	if (rtHeld) {
		armRight = clamp(armRight - GP_ARM_STEP, 0, 100);
		evaluateCommand('R', int(armRight));
	}

	// -- Buttons (edge-detected) -------------------------------------
	uint16_t buttons = gamepad->buttons();
	uint8_t  dpad = gamepad->dpad();
	uint8_t  misc = gamepad->miscButtons();

	handleButtons(buttons & ~prevButtons);
	handleDpad(dpad & ~prevDpad);
	handleMisc(misc & ~prevMisc);

	prevButtons = buttons;
	prevDpad = dpad;
	prevMisc = misc;
}

#endif /* BT_GAMEPAD_ENABLED */


// -------------------------------------------------------------------
/// Public interface
// -------------------------------------------------------------------

void btGamepadInit() {
#if BT_GAMEPAD_ENABLED
	BP32.setup(&onConnected, &onDisconnected);
	// Allow new gamepads to pair at any time; remove stored keys so a
	// previously paired different gamepad doesn't block new pairings
	BP32.forgetBluetoothKeys();
	Serial.println(F("Bluetooth gamepad started (Bluepad32)"));
#else
	Serial.println(F("Bluetooth gamepad disabled (BT_GAMEPAD_ENABLED=0)"));
#endif
}

void btGamepadLoop() {
#if BT_GAMEPAD_ENABLED
	BP32.update();

	if (gamepad == nullptr || !gamepad->isConnected()) return;

	unsigned long now = millis();
	if (now - lastPoll < GP_POLL_INTERVAL_MS) return;
	lastPoll = now;

	pollGamepad();
#endif
}

bool btGamepadIsConnected() {
#if BT_GAMEPAD_ENABLED
	return connected && gamepad != nullptr && gamepad->isConnected();
#else
	return false;
#endif
}

bool btGamepadIsActive() {
#if BT_GAMEPAD_ENABLED
	return true;
#else
	return false;
#endif
}
