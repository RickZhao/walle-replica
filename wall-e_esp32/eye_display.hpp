/**
 * EYE DISPLAYS (2x GC9A01 1.28" round, 240x240)
 *
 * @file    eye_display.hpp
 * @brief   Drives the two round GC9A01 displays from the third-party
 *          BOM as Wall-E's eyes. Renders vector-drawn eyes (no bitmap
 *          assets needed): iris + pupil + eyelid, with expressions
 *          and periodic blinking.
 *
 * Expressions follow the same commands as the physical eye servos
 * ('i' sad, 'j' tilt left, 'l' tilt right, 'k' neutral), hooked in
 * evaluateCommand() so all control paths (serial / HTTP / gamepad)
 * update the displays automatically.
 *
 * Requires the Arduino_GFX library (moononournation).
 * Enable/disable and pin mapping: web_config.h (DISPLAYS_ENABLED).
 */

#ifndef EYE_DISPLAY_HPP
#define EYE_DISPLAY_HPP

enum EyeExpression {
	EYE_NEUTRAL,
	EYE_SAD,
	EYE_TILT_LEFT,
	EYE_TILT_RIGHT,
};

/// Initialise both GC9A01 displays and draw the neutral eyes.
/// Call once from setup(). No-op when DISPLAYS_ENABLED is 0.
void eyeDisplayInit();

/// Blink animation and timing. Call on every iteration of loop().
void eyeDisplayLoop();

/// Change the current expression (redraws both eyes).
void eyeDisplaySetExpression(EyeExpression expression);

#endif /* EYE_DISPLAY_HPP */
