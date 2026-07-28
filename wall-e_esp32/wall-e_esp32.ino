/**
 * WALL-E CONTROLLER CODE - ESP32-S3 PORT
 *
 * @file       wall-e_esp32.ino
 * @brief      Main Wall-E Controller Sketch, ported from Arduino UNO to ESP32-S3
 * @author     Simon Bluett (original), ESP32-S3 port by this repository
 * @copyright  Copyright (C) 2021 - Distributed under MIT license
 * @version    2.9-esp32
 *
 * This is a straight port of wall-e/wall-e.ino. The serial protocol is
 * IDENTICAL to the UNO version (see docs/SERIAL_PROTOCOL.md), so the
 * Raspberry Pi web interface (web_interface/) works unchanged: connect
 * the ESP32-S3 over USB and point config.py at its serial port.
 *
 * HOW TO USE:
 * 1. Install the "esp32" board package (Arduino-ESP32 core 3.x) and the
 *    Adafruit_PWMServoDriver library.
 * 2. In the Arduino IDE select your ESP32-S3 board and set
 *    Tools -> USB CDC On Boot -> Enabled (Serial = native USB).
 * 3. Calibrate the servo motors with the wall-e_esp32_calibration sketch and
 *    paste the resulting values into the preset array below. If you keep the
 *    same servo driver board (60Hz) and mechanics, existing preset values
 *    from the UNO version carry over directly.
 * 4. Upload and open the serial monitor at 115200 baud.
 *
 * HARDWARE NOTES (third-party BOM, see hardware/另一套硬件方案.md):
 * - LU9685 servo driver boards are usually register-compatible with the
 *   PCA9685; the Adafruit library drives them at the default address 0x40.
 *   If your board uses a different address, pass it to the constructor below.
 * - The TB6612FNG STBY pin is driven from MOTOR_STBY_PIN; tie STBY to 3.3V
 *   instead if you want to free that GPIO.
 * - Avoid GPIO 0/3/45/46 (strapping), 19/20 (USB), 26-32 (flash) and
 *   33-37 (octal PSRAM on N16R8 modules).
 */

#include <Wire.h>
#include <esp_random.h>
#include <Adafruit_PWMServoDriver.h>
#include "Queue.hpp"
#include "MotorController.hpp"
#include "web_server.hpp"
#include "audio_player.hpp"
#include "bt_gamepad.hpp"


/// Define pin-mapping (ESP32-S3)
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
// Uncomment one of the following lines to select your motor driver:
#define MOTOR_DRIVER_TB6612FNG       // Dual-direction-pin driver (TB6612FNG)
// #define MOTOR_DRIVER_ARDUINO_SHIELD // Original Arduino Motor Shield Rev2

#define DIRECTION_L_PIN  5           // Motor direction pins (TB6612: AIN1/BIN1)
#define DIRECTION_R_PIN  15
#define PWM_SPEED_L_PIN  4           // Motor PWM pins (TB6612: PWMA/PWMB)
#define PWM_SPEED_R_PIN  7
#define MOTOR_STBY_PIN   17          // TB6612 standby pin (driven HIGH)
#define SERVO_ENABLE_PIN 10          // Servo driver board output enable (OE) pin

#define I2C_SDA_PIN      8           // I2C pins for the servo driver board
#define I2C_SCL_PIN      9           // (and the optional oLED display)

#ifdef MOTOR_DRIVER_TB6612FNG
	#define DIRECTION_L_PIN2 6       // Second complementary direction pin (TB6612: AIN2/BIN2)
	#define DIRECTION_R_PIN2 16
#else
	#define BRAKE_L_PIN  6           // Motor brake pins (Arduino Motor Shield Rev2 only)
	#define BRAKE_R_PIN  16
#endif


/**
 * Battery level detection
 *
 *   .------R1-----.-------R2------.     | The diagram to the left shows the  |
 *   |             |               |     | potential divider circuit used by  |
 * V_Raw     Analogue pin         GND    | the battery level detection system |
 *
 * @note The scaling factor is calculated according to ratio of the two resistors:
 *       DIVIDER_SCALING_FACTOR = R2 / (R1 + R2)
 *
 * @warning The ESP32 ADC reads up to ~3.1V (ADC_11db attenuation), NOT 5V like
 *       the UNO. The divider must bring a full battery (12.6V) below ~3.1V,
 *       e.g. R1=100k + R2=22k gives 22/122 = 0.180 and 12.6V -> 2.27V.
 *       The original UNO divider (0.3197) would exceed the ADC range.
 *
 * To enable battery level detection, uncomment the next line:
 */
//#define BAT_L
#ifdef BAT_L
	#define BATTERY_LEVEL_PIN 1      // GPIO1 (ADC1_CH0)
	#define BATTERY_MAX_VOLTAGE 12.6
	#define BATTERY_MIN_VOLTAGE 10.2
	#define DIVIDER_SCALING_FACTOR 0.180


	/**
	 * OLED Battery Level Display
	 *
	 * Displays the battery level on an oLed display. Supports a 1.3 inch oLed display using I2C.
	 * The constructor is set to a SH1106 1.3 inch display. Change the constructor if you want to use a different display.
	 *
	 * @note Requires Battery level detection to be enabled above
	 *
	 * To enable the oLED display, uncomment the next line:
	 */
	//#define OLED
	#ifdef OLED

	  #include <U8g2lib.h>
	  U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

	#endif /* OLED */
#endif /* BAT_L */


/// Define other constants
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
#define NUMBER_OF_SERVOS 9        // Number of servo motors
#define SERVO_UPDATE_TIME 10      // Time in milliseconds of how often to update servo and motor positions
#define SERVO_OFF_TIME 6000       // Turn servo motors off after 6 seconds
#define STATUS_CHECK_TIME 10000   // Time in milliseconds of how often to check robot status (eg. battery level)
#define CONTROLLER_THRESHOLD 1    // The minimum error which the dynamics controller tries to achieve
#define MAX_SERIAL_LENGTH 5       // Maximum number of characters that can be received



/// Instantiate Objects
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
// Servo driver controller class - assumes default address 0x40
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();

// Set up motor controller classes
#ifdef MOTOR_DRIVER_TB6612FNG
	MotorController motorL(DIRECTION_L_PIN, DIRECTION_L_PIN2, PWM_SPEED_L_PIN);
	MotorController motorR(DIRECTION_R_PIN, DIRECTION_R_PIN2, PWM_SPEED_R_PIN);
#else
	MotorController motorL(DIRECTION_L_PIN, PWM_SPEED_L_PIN, BRAKE_L_PIN, false);
	MotorController motorR(DIRECTION_R_PIN, PWM_SPEED_R_PIN, BRAKE_R_PIN, false);
#endif

// Queue for animations - buffer is defined outside of the queue class
// so that the compiler knows how much dynamic memory will be used
struct animation_t {
	uint16_t timer;
	int8_t servos[NUMBER_OF_SERVOS];
};

#define QUEUE_LENGTH 40
animation_t buffer[QUEUE_LENGTH];
Queue <animation_t> queue(QUEUE_LENGTH, buffer);


/// Motor Control Variables
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
int pwmspeed = 255;
int moveValue = 0;
int turnValue = 0;
int turnOffset = 0;
int motorDeadzone = 0;


/// Runtime Variables
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
unsigned long lastTime = 0;
unsigned long animeTimer = 0;
unsigned long motorTimer = 0;
unsigned long statusTimer = 0;
unsigned long updateTimer = 0;
bool autoMode = false;
int batteryLevel = -999;      // Last measured battery %; -999 = no reading (web UI hides the icon)


// Serial Parsing
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
char firstChar;
char serialBuffer[MAX_SERIAL_LENGTH];
uint8_t serialLength = 0;


// ****** SERVO MOTOR CALIBRATION *********************
// Servo Positions:  Low,High
int preset[][2] =  {{410,120},  // head rotation
                    {532,178},  // neck top
                    {120,310},  // neck bottom
                    {465,271},  // eye right
                    {278,479},  // eye left
                    {340,135},  // arm left
                    {150,360},  // arm right
                    {150,600},  // eyebrow left  (placeholder - run calibration sketch)
                    {150,600}}; // eyebrow right (placeholder - run calibration sketch)
// *****************************************************


// Physical PWM channel (on the LU9685/PCA9685 board) for each logical joint.
// Logical joint order (used by preset, curpos/setpos and all animations):
//   0=head, 1=necT, 2=necB, 3=eyeR, 4=eyeL, 5=armL, 6=armR, 7=broL, 8=broR
// Third-party wiring harness channel order (hardware/另一套硬件方案.md):
//   0=eyeL, 1=eyeR, 2=head rotation, 3=head up/down (necT),
//   4=head extend/retract (necB), 5=armL, 6=armR, 7=broL, 8=broR
// Rewire the servos or edit this array if your harness differs.
uint8_t servoChannel[NUMBER_OF_SERVOS] = {2, 3, 4, 1, 0, 5, 6, 7, 8};


// Servo Control - Position, Velocity, Acceleration
// -- -- -- -- -- -- -- -- -- -- -- -- -- --
// Servo Pins:	     0,   1,   2,   3,   4,   5,   6,   7,   8,   -,   -
// Joint Name:	  head,necT,necB,eyeR,eyeL,armL,armR,broL,broR,motL,motR
float curpos[] = { 248, 560, 140, 475, 270, 250, 290, 375, 375, 180, 180};  // Current position (units)
float setpos[] = { 248, 560, 140, 475, 270, 250, 290, 375, 375,   0,   0};  // Required position (units)
float curvel[] = {   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0};  // Current velocity (units/sec)
float maxvel[] = { 500, 400, 500,2400,2400, 600, 600,1200,1200, 255, 255};  // Max Servo velocity (units/sec)
float accell[] = { 350, 300, 480,1800,1800, 500, 500, 900, 900, 800, 800};  // Servo acceleration (units/sec^2)



// -------------------------------------------------------------------
/// Initial setup
// -------------------------------------------------------------------

void setup() {

	// ESP32 motor PWM (LEDC via analogWrite): 8-bit duty (0-255) at 20kHz,
	// above the audible range, so the motors don't whine
	analogWriteResolution(8);
	analogWriteFrequency(20000);

	// TB6612 standby pin - keep the driver out of standby
	pinMode(MOTOR_STBY_PIN, OUTPUT);
	digitalWrite(MOTOR_STBY_PIN, HIGH);

	// Output Enable (EO) pin for the servo motors
	pinMode(SERVO_ENABLE_PIN, OUTPUT);
	digitalWrite(SERVO_ENABLE_PIN, HIGH);

	// ESP32: route I2C to the chosen pins before initialising the PWM driver
	Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

	// Communicate with servo driver board (Analog servos run at ~60Hz)
	pwm.begin();
	pwm.setPWMFreq(60);

	// Turn off servo outputs
	for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
		pwm.setPin(i, 0);
	}

	// Initialize serial communication for debugging
	Serial.begin(115200);
	Serial.println(F("--- Wall-E Control Sketch (ESP32-S3) ---"));

	// Seed the PRNG from the ESP32 hardware random number generator
	randomSeed(esp_random());

	// Check if servo animation queue is working, and move servos to known starting positions
	if (queue.errors()) Serial.println(F("Error: Unable to allocate memory for servo animation queue"));

	// Soft start the servo motors
	Serial.println(F("Starting up the servo motors"));
	digitalWrite(SERVO_ENABLE_PIN, LOW);
	playAnimation(0);
	softStart(queue.pop(), 3500);

	// If an oLED is present, start it up
	#ifdef OLED
		Serial.println(F("Starting up the display"));
		u8g2.begin();
		displayLevel(100);
	#endif

	// Prepare the battery ADC pin
	#ifdef BAT_L
		analogSetPinAttenuation(BATTERY_LEVEL_PIN, ADC_11db);
	#endif

	// Bring up the Bluetooth gamepad, Wi-Fi + HTTP web interface,
	// and the I2S audio player (LittleFS is mounted by webServerInit,
	// so the audio player must come after it)
	btGamepadInit();
	webServerInit();
	audioPlayerInit();

	Serial.println(F("Startup complete; entering main loop"));
}



// -------------------------------------------------------------------
/// Read input from serial port
///
/// This function reads incoming characters in the serial port
/// and inserts them into a buffer to be processed later.
// -------------------------------------------------------------------

void readSerial() {

	// Read incoming byte
	char inchar = Serial.read();

	// If the string has ended, evaluate the serial buffer
	if (inchar == '\n' || inchar == '\r') {

		if (serialLength > 0) evaluateSerial();
		serialBuffer[0] = 0;
		serialLength = 0;

	// Otherwise add to the character to the buffer
	} else {
		if (serialLength == 0) firstChar = inchar;
		else {
			serialBuffer[serialLength-1] = inchar;
			serialBuffer[serialLength] = 0;
		}
		serialLength++;

		// To prevent overflows, evalute the buffer if it is full
		if (serialLength == MAX_SERIAL_LENGTH) {
			evaluateSerial();
			serialBuffer[0] = 0;
			serialLength = 0;
		}
	}
}



// -------------------------------------------------------------------
/// Evaluate input from serial port
///
/// Parse the received serial message which is stored in
/// the "serialBuffer" filled by the "readSerial()" function
// -------------------------------------------------------------------

void evaluateSerial() {

	// Evaluate integer number in the serial buffer, then dispatch
	evaluateCommand(firstChar, atoi(serialBuffer));
}



// -------------------------------------------------------------------
/// Evaluate a single command
///
/// Shared command dispatcher, called by BOTH the serial parser
/// (evaluateSerial) and the HTTP web server (web_server.cpp), so the
/// behaviour of the USB serial and Wi-Fi control paths is identical.
///
/// @param  prefix  Command prefix character (see docs/SERIAL_PROTOCOL.md)
/// @param  number  Numeric argument of the command
// -------------------------------------------------------------------

void evaluateCommand(char prefix, int number) {

	Serial.print(prefix); Serial.println(number);


	// Motor Inputs and Offsets
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	if      (prefix == 'X' && number >= -100 && number <= 100) turnValue = int(number * 2.55);       // Left/right control
	else if (prefix == 'Y' && number >= -100 && number <= 100) moveValue = int(number * 2.55);       // Forward/reverse control
	else if (prefix == 'S' && number >= -100 && number <= 100) turnOffset = number;                  // Steering offset
	else if (prefix == 'O' && number >=    0 && number <= 250) motorDeadzone = int(number);          // Motor deadzone offset


	// Animations
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'A') playAnimation(number);


	// Autonomous servo mode
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'M' && number == 0) autoMode = false;
	else if (prefix == 'M' && number == 1) autoMode = true;


	// Manual servo control
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'L' && number >= 0 && number <= 100) {   // Move left arm
		autoMode = false;
		queue.clear();
		setpos[5] = int(number * 0.01 * (preset[5][1] - preset[5][0]) + preset[5][0]);
	} else if (prefix == 'R' && number >= 0 && number <= 100) { // Move right arm
		autoMode = false;
		queue.clear();
		setpos[6] = int(number * 0.01 * (preset[6][1] - preset[6][0]) + preset[6][0]);
	} else if (prefix == 'B' && number >= 0 && number <= 100) { // Move neck bottom
		autoMode = false;
		queue.clear();
		setpos[2] = int(number * 0.01 * (preset[2][1] - preset[2][0]) + preset[2][0]);
	} else if (prefix == 'T' && number >= 0 && number <= 100) { // Move neck top
		autoMode = false;
		queue.clear();
		setpos[1] = int(number * 0.01 * (preset[1][1] - preset[1][0]) + preset[1][0]);
	} else if (prefix == 'G' && number >= 0 && number <= 100) { // Move head rotation
		autoMode = false;
		queue.clear();
		setpos[0] = int(number * 0.01 * (preset[0][1] - preset[0][0]) + preset[0][0]);
	} else if (prefix == 'E' && number >= 0 && number <= 100) { // Move eye left
		autoMode = false;
		queue.clear();
		setpos[4] = int(number * 0.01 * (preset[4][1] - preset[4][0]) + preset[4][0]);
	} else if (prefix == 'U' && number >= 0 && number <= 100) { // Move eye right
		autoMode = false;
		queue.clear();
		setpos[3] = int(number * 0.01 * (preset[3][1] - preset[3][0]) + preset[3][0]);
	} else if (prefix == 'I' && number >= 0 && number <= 100) { // Move eyebrow left
		autoMode = false;
		queue.clear();
		setpos[7] = int(number * 0.01 * (preset[7][1] - preset[7][0]) + preset[7][0]);
	} else if (prefix == 'J' && number >= 0 && number <= 100) { // Move eyebrow right
		autoMode = false;
		queue.clear();
		setpos[8] = int(number * 0.01 * (preset[8][1] - preset[8][0]) + preset[8][0]);
	}


	// Manual Movements with WASD
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'w') {		// Forward movement
		moveValue = pwmspeed;
		turnValue = 0;
		setpos[0] = (preset[0][1] + preset[0][0]) / 2;
	}
	else if (prefix == 'q') {		// Stop movement
		moveValue = 0;
		turnValue = 0;
		setpos[0] = (preset[0][1] + preset[0][0]) / 2;
	}
	else if (prefix == 's') {		// Backward movement
		moveValue = -pwmspeed;
		turnValue = 0;
		setpos[0] = (preset[0][1] + preset[0][0]) / 2;
	}
	else if (prefix == 'a') {		// Drive & look left
		moveValue = 0;
		turnValue = -pwmspeed;
		setpos[0] = preset[0][0];
	}
	else if (prefix == 'd') {   		// Drive & look right
		moveValue = 0;
		turnValue = pwmspeed;
		setpos[0] = preset[0][1];
	}


	// Manual Eye Movements
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'j') {		// Left head tilt
		setpos[4] = preset[4][0];
		setpos[3] = preset[3][1];
	}
	else if (prefix == 'l') {		// Right head tilt
		setpos[4] = preset[4][1];
		setpos[3] = preset[3][0];
	}
	else if (prefix == 'i') {		// Sad head
		setpos[4] = preset[4][0];
		setpos[3] = preset[3][0];
	}
	else if (prefix == 'k') {		// Neutral head
		setpos[4] = int(0.4 * (preset[4][1] - preset[4][0]) + preset[4][0]);
		setpos[3] = int(0.4 * (preset[3][1] - preset[3][0]) + preset[3][0]);
	}


	// Head movement
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'f') {		// Head up
		setpos[1] = preset[1][0];
		setpos[2] = (preset[2][1] + preset[2][0])/2;
	}
	else if (prefix == 'g') {		// Head forward
		setpos[1] = preset[1][1];
		setpos[2] = preset[2][0];
	}
	else if (prefix == 'h') {		// Head down
		setpos[1] = preset[1][0];
		setpos[2] = preset[2][0];
	}


	// Arm Movements
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	else if (prefix == 'b') {		// Left arm low, right arm high
		setpos[5] = preset[5][0];
		setpos[6] = preset[6][1];
	}
	else if (prefix == 'n') {		// Both arms neutral
		setpos[5] = (preset[5][0] + preset[5][1]) / 2;
		setpos[6] = (preset[6][0] + preset[6][1]) / 2;
	}
	else if (prefix == 'm') {		// Left arm high, right arm low
		setpos[5] = preset[5][1];
		setpos[6] = preset[6][0];
	}
}



// -------------------------------------------------------------------
/// Sequence and generate animations
// -------------------------------------------------------------------

void manageAnimations() {

	// If we are running an animation
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	if ((queue.size() > 0) && (animeTimer <= millis())) {
		// Set the next waypoint time
		animation_t newValues = queue.pop();
		animeTimer = millis() + newValues.timer;

		// Set all the joint positions
		for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
			// Scale the positions using the servo calibration values
			setpos[i] = int(newValues.servos[i] * 0.01 * (preset[i][1] - preset[i][0]) + preset[i][0]);
		}


	// If we are in autonomous mode and no movements are queued, generate random movements
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	} else if (autoMode && queue.empty() && (animeTimer <= millis())) {

		// For each of the servos
		for (int i = 0; i < NUMBER_OF_SERVOS; i++) {

			// Randomly determine whether or not to update the servo
			if (random(2) == 1) {

				// For most of the servo motors
				if (i == 0 || i == 1 || i == 5 || i == 6) {

					// Randomly determine the new position
					unsigned int min = preset[i][0];
					unsigned int max = preset[i][1];
					if (min > max) {
						min = max;
						max = preset[i][0];
					}

					setpos[i] = random(min, max+1);

				// Since the eyes should work together, only look at one of them
				} else if (i == 3) {

					int midPos1 = int((preset[i][1] - preset[i][0])*0.4 + preset[i][0]);
					int midPos2 = int((preset[i+1][1] - preset[i+1][0])*0.4 + preset[i+1][0]);

					// Determine which type of eye movement to do
					// Both eye move downwards
					if (random(2) == 1) {
						setpos[i] = random(midPos1, preset[i][0]);
						float multiplier = (setpos[i] - midPos1) / float(preset[i][0] - midPos1);
						setpos[i+1] = ((1 - multiplier) * (midPos2 - preset[i+1][0])) + preset[i+1][0];

					// Both eyes move in opposite directions
					} else {
						setpos[i] = random(midPos1, preset[i][0]);
						float multiplier = (setpos[i] - preset[i][1]) / float(preset[i][0] - preset[i][1]);
						setpos[i+1] = (multiplier * (preset[i+1][1] - preset[i+1][0])) + preset[i+1][0];
					}
				}

			}
		}

		// Finally, figure out the amount of time until the next movement should be done
		animeTimer = millis() + random(500, 3000);

	}
}



// -------------------------------------------------------------------
/// Manage the movement of the servo motors
///
/// @param  dt  Time in milliseconds since function was last called
///
/// This function uses the formulae:
///   (s = position, v = velocity, a = acceleration, t = time)
///   s = v^2 / (2*a)  <- to figure out whether to start slowing down
///   v = v + a*t      <- to calculate new servo velocity
///   s = s + v*t      <- to calculate new servo position
// -------------------------------------------------------------------

void manageServos(float dt) {

	bool moving = false;

	// For each of the servo motors
	for (int i = 0; i < NUMBER_OF_SERVOS; i++) {

		float posError = setpos[i] - curpos[i];

		// If position error is above the threshold
		if (abs(posError) > CONTROLLER_THRESHOLD && (setpos[i] != -1)) {

			digitalWrite(SERVO_ENABLE_PIN, LOW);
			moving = true;

			// Determine motion direction
			bool dir = true;
			if (posError < 0) dir = false;

			// Determine whether to accelerate or decelerate
			float acceleration = accell[i];
			if ((curvel[i] * curvel[i] / (2 * accell[i])) > abs(posError)) acceleration = -accell[i];

			// Update the current velocity
			if (dir) curvel[i] += acceleration * dt / 1000.0;
			else curvel[i] -= acceleration * dt / 1000.0;

			// Limit Velocity
			if (curvel[i] > maxvel[i]) curvel[i] = maxvel[i];
			if (curvel[i] < -maxvel[i]) curvel[i] = -maxvel[i];

			float dP = curvel[i] * dt / 1000.0;

			if (abs(dP) < abs(posError)) curpos[i] += dP;
			else curpos[i] = setpos[i];

			pwm.setPWM(servoChannel[i], 0, curpos[i]);

		} else {
			curvel[i] = 0;
		}
	}

	// Disable servos if robot is not moving
	// This helps prevents the motors from overheating
	if (moving) motorTimer = millis();
	else if (millis() - motorTimer >= SERVO_OFF_TIME) {
		//digitalWrite(SERVO_ENABLE_PIN, HIGH);
		for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
			pwm.setPin(servoChannel[i], 0);
		}
	}
}



// -------------------------------------------------------------------
/// Servo "Soft Start" function
///
/// This function tries to start the servos up servo gently,
/// reducing the sudden jerking motion which usually occurs
/// when the motors power up for the first time.
///
/// @param  targetPos  The target position of the servos after startup
/// @param  timeMs     Time in milliseconds in which soft start should complete
// -------------------------------------------------------------------

void softStart(animation_t targetPos, int timeMs) {

	for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
		if (targetPos.servos[i] >= 0) {
			curpos[i] = int(targetPos.servos[i] * 0.01 * (preset[i][1] - preset[i][0]) + preset[i][0]);

			unsigned long endTime = millis() + timeMs / NUMBER_OF_SERVOS;

			while (millis() < endTime) {
				pwm.setPWM(servoChannel[i], 0, curpos[i]);
				delay(10);
				pwm.setPin(servoChannel[i], 0);
				delay(50);
			}
			pwm.setPWM(servoChannel[i], 0, curpos[i]);
			setpos[i] = curpos[i];
		}
	}
}



// -------------------------------------------------------------------
/// Manage the movement of the main motors
///
/// @param  dt  Time in milliseconds since function was last called
// -------------------------------------------------------------------

void manageMotors(float dt) {

	// Update Main Motor Values
	setpos[NUMBER_OF_SERVOS] = moveValue - turnValue;
	setpos[NUMBER_OF_SERVOS + 1] = moveValue + turnValue;

	// Apply turn offset (motor trim) only when motors are active
	if (setpos[NUMBER_OF_SERVOS] != 0) setpos[NUMBER_OF_SERVOS] -= turnOffset;
	if (setpos[NUMBER_OF_SERVOS + 1] != 0) setpos[NUMBER_OF_SERVOS + 1] += turnOffset;

	for (int i = NUMBER_OF_SERVOS; i < NUMBER_OF_SERVOS + 2; i++) {

		float velError = setpos[i] - curvel[i];

		// If velocity error is above the threshold
		if (abs(velError) > CONTROLLER_THRESHOLD && (setpos[i] != -1)) {

			// Determine whether to accelerate or decelerate
			float acceleration = accell[i];
			if (setpos[i] < curvel[i] && curvel[i] >= 0) acceleration = -accell[i];
			else if (setpos[i] < curvel[i] && curvel[i] < 0) acceleration = -accell[i];
			else if (setpos[i] > curvel[i] && curvel[i] < 0) acceleration = accell[i];

			// Update the current velocity
			float dV = acceleration * dt / 1000.0;
			if (abs(dV) < abs(velError)) curvel[i] += dV;
			else curvel[i] = setpos[i];
		} else {
			curvel[i] = setpos[i];
		}

		// Apply deadzone offset
		if (curvel[i] > 0) curvel[i] += motorDeadzone;
		else if (curvel[i] < 0) curvel[i] -= motorDeadzone;

		// Limit Velocity
		if (curvel[i] > maxvel[i]) curvel[i] = maxvel[i];
		if (curvel[i] < -maxvel[i]) curvel[i] = -maxvel[i];
	}

	// Update motor speeds
	motorL.setSpeed(curvel[NUMBER_OF_SERVOS]);
	motorR.setSpeed(curvel[NUMBER_OF_SERVOS+1]);
}



// -------------------------------------------------------------------
/// Battery level detection
// -------------------------------------------------------------------

#ifdef BAT_L
void checkBatteryLevel() {

	// Read the analogue pin and calculate battery voltage.
	// ESP32: use calibrated millivolt readings instead of the UNO's 5V/1024 math.
	float voltage = analogReadMilliVolts(BATTERY_LEVEL_PIN) / 1000.0;
	voltage = voltage / DIVIDER_SCALING_FACTOR;
	int percentage = int(100 * (voltage - BATTERY_MIN_VOLTAGE) / float(BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE));

	// Clamp to 0-100% (the UNO version doesn't clamp, but out-of-range
	// divider values are more likely with the smaller ESP32 ADC range)
	if (percentage > 100) percentage = 100;
	if (percentage < 0) percentage = 0;

	// Store for the web interface (/arduinoStatus route)
	batteryLevel = percentage;

  // Update the oLed Display if installed
  #ifdef OLED
    displayLevel(percentage);
  #endif

	// Send the percentage via serial
	Serial.print(F("Battery_")); Serial.println(percentage);
}
#endif



// -------------------------------------------------------------------
/// Main program loop
// -------------------------------------------------------------------

void loop() {

	// Handle pending web server actions (e.g. delayed restart),
	// feed the audio decoder and poll the Bluetooth gamepad
	webServerLoop();
	audioPlayerLoop();
	btGamepadLoop();

	// Read any new serial messages
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	if (Serial.available() > 0){
		readSerial();
	}


	// Load or generate new animations
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	manageAnimations();


	// Move Servos and wheels at regular time intervals
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	if (millis() - updateTimer >= SERVO_UPDATE_TIME) {
		updateTimer = millis();

		unsigned long newTime = micros();
		float dt = (newTime - lastTime) / 1000.0;
		lastTime = newTime;

		manageServos(dt);
		manageMotors(dt);
	}


	// Update robot status
	// -- -- -- -- -- -- -- -- -- -- -- -- -- --
	if (millis() - statusTimer >= STATUS_CHECK_TIME) {
		statusTimer = millis();

		#ifdef BAT_L
			checkBatteryLevel();
		#endif
	}
}
