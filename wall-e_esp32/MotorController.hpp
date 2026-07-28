/* * * * * * * * * * * * * * * * * * * * * * *
 * MOTOR CONTROLLER CLASS
 * For the Arduino Motor Shield Rev.2
 *
 * Code by:  Simon Bluett
 * Email:    hello@chillibasket.com
 * Version:  1.1
 * Date:     20th June 2020
 * Copyright (C) 2020, MIT License
 * * * * * * * * * * * * * * * * * * * * * * */

#ifndef MOTOR_CONTROLLER_HPP
#define MOTOR_CONTROLLER_HPP

// MOTOR CONTROLLER CLASS
class MotorController {
public:
	// Constructors
	// Traditional: single DIR pin + brake pin (e.g. Arduino Motor Shield Rev2)
	MotorController(uint8_t _dirPin, uint8_t _pwmPin, uint8_t _brkPin, bool _brkEnabled);
	// Dual-direction: two complementary DIR pins, no brake pin (e.g. TB6612FNG)
	MotorController(uint8_t _dirPin, uint8_t _dirPin2, uint8_t _pwmPin);
	
	// Functions
	void setSpeed(int pwmValue);

	// Default destructor
	~MotorController();

private:
	uint8_t dirPin, dirPin2, pwmPin, brkPin;
	bool reverse, brake, brakeEnabled, dualDir;
};


/**
 * Traditional Constructor (single DIR + brake)
 * 
 * @param  (_dirPin) Digital pin used for motor direction
 * @param  (_pwmPin) Digital pin for PWM motor speed control
 * @param  (_brkPin) Digital pin to enable/disable the breaks
 * @param  (_brkEnabled) Should the break be used?
 */
MotorController::MotorController(uint8_t _dirPin, uint8_t _pwmPin, uint8_t _brkPin, bool _brkEnabled) {
	dirPin = _dirPin;
	dirPin2 = 255;
	pwmPin = _pwmPin;
	brkPin = _brkPin;
	brakeEnabled = _brkEnabled;
	dualDir = false;

	pinMode(dirPin, OUTPUT);     // Motor Direction
	pinMode(brkPin, OUTPUT);     // Motor Brake
	digitalWrite(dirPin, HIGH);

	reverse = false;
	if (brakeEnabled) {
		digitalWrite(brkPin, HIGH);
		brake = true;
	} else {
		digitalWrite(brkPin, LOW);
		brake = false;
	}
}


/**
 * Dual-Direction Constructor (two complementary DIR pins, no brake)
 * 
 * @param  (_dirPin)  Digital pin for direction 1
 * @param  (_dirPin2) Digital pin for direction 2 (complementary)
 * @param  (_pwmPin)  Digital pin for PWM motor speed control
 * @note   For drivers like TB6612FNG that require IN1/IN2 logic levels.
 */
MotorController::MotorController(uint8_t _dirPin, uint8_t _dirPin2, uint8_t _pwmPin) {
	dirPin = _dirPin;
	dirPin2 = _dirPin2;
	pwmPin = _pwmPin;
	brkPin = 255;
	brakeEnabled = false;
	dualDir = true;

	pinMode(dirPin, OUTPUT);
	pinMode(dirPin2, OUTPUT);
	// Initial state: forward
	digitalWrite(dirPin, HIGH);
	digitalWrite(dirPin2, LOW);

	reverse = false;
	brake = false;
}


/**
 * Default Destructor
 */
MotorController::~MotorController() {
	// Empty
}


/**
 * Set a new motor speed
 * 
 * @param  (pwmValue) The PWM value of the new speed
 * @note   Negative PWM values will cause the motor to move in reverse
 * @note   A PWM value of 0 will enable the breaks
 * @note   In dual-direction mode (TB6612FNG) both direction pins are
 *         rewritten on every update: the brake state (both pins HIGH)
 *         would otherwise latch after a stop, since the single-DIR
 *         logic below only rewrites the pins when the direction
 *         changes, leaving the motor braked forever.
 */
void MotorController::setSpeed(int pwmValue) {

	// Bound the PWM value to +-255
	if (pwmValue > 255) pwmValue = 255;
	else if (pwmValue < -255) pwmValue = -255;

	// Dual-direction mode (TB6612FNG): set both direction pins explicitly
	if (dualDir) {
		if (pwmValue > 0) {
			digitalWrite(dirPin, HIGH);
			digitalWrite(dirPin2, LOW);
		} else if (pwmValue < 0) {
			digitalWrite(dirPin, LOW);
			digitalWrite(dirPin2, HIGH);
		} else {
			// No movement: engage the short brake
			digitalWrite(dirPin, HIGH);
			digitalWrite(dirPin2, HIGH);
		}
		analogWrite(pwmPin, abs(pwmValue));
		return;
	}
	
	// Forward direction
	if (pwmValue > 0 && reverse) {
		digitalWrite(dirPin, HIGH);
		if (dualDir) digitalWrite(dirPin2, LOW);
		reverse = false;

		// Release the brake
		if (brake) {
			digitalWrite(brkPin, LOW);
			brake = false;
		}

	// Reverse direction
	} else if (pwmValue < 0 && !reverse) {
		digitalWrite(dirPin, LOW);
		if (dualDir) digitalWrite(dirPin2, HIGH);
		reverse = true;

		// Release the brake
		if (brake) {
			digitalWrite(brkPin, LOW);
			brake = false;
		}

	// If there is no movement, engage the brake
	} else if (brakeEnabled && !brake) {
		digitalWrite(brkPin, HIGH);
		brake = true;
	}
	
	// Send PWM value
	analogWrite(pwmPin, abs(pwmValue));
}


#endif /* MOTOR_CONTROLLER_HPP */