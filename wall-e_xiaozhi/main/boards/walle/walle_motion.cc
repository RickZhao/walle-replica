/**
 * WALL-E MOTION CORE (ESP-IDF port of wall-e_esp32.ino)
 *
 * @file    walle_motion.cc
 * @brief   Implementation, see walle_motion.h
 *
 * Control loop runs in its own FreeRTOS task at 100Hz (10ms period),
 * priority 5 (below the audio pipeline to avoid Opus underruns).
 * Timing uses esp_timer (microseconds), replacing Arduino millis/micros.
 */

#include "walle_motion.h"
#include "pca9685.h"
#include "config.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstdio>

#define TAG "WalleMotion"

// -- Constants (identical to the Arduino firmware) --------------------
#define NUMBER_OF_SERVOS    9
#define SERVO_UPDATE_MS     10
#define SERVO_OFF_MS        6000
#define STATUS_CHECK_MS     10000
#define CONTROLLER_THRESHOLD 1

#define MOTOR_PWM_FREQ_HZ   20000
#define MOTOR_PWM_BITS      LEDC_TIMER_8_BIT

#define BATTERY_MAX_VOLTAGE 12.6f
#define BATTERY_MIN_VOLTAGE 10.2f
#define BATTERY_DIVIDER     0.180f     // R2/(R1+R2), e.g. 100k+22k


// Logical joint order: head, necT, necB, eyeR, eyeL, armL, armR, broL, broR
static const int kNumberOfJoints = 11;   // 9 servos + 2 motors

// ****** SERVO MOTOR CALIBRATION (paste calibration results here) *****
// Servo Positions:  Low,High
static int preset[NUMBER_OF_SERVOS][2] = {
    {410,120},  // head rotation
    {532,178},  // neck top
    {120,310},  // neck bottom
    {465,271},  // eye right
    {278,479},  // eye left
    {340,135},  // arm left
    {150,360},  // arm right
    {150,600},  // eyebrow left  (placeholder - calibrate!)
    {150,600},  // eyebrow right (placeholder - calibrate!)
};

// Physical PWM channel for each logical joint (third-party harness):
// channels: 0=eyeL, 1=eyeR, 2=head, 3=necT, 4=necB, 5=armL, 6=armR, 7=broL, 8=broR
static const uint8_t servo_channel[NUMBER_OF_SERVOS] = {2, 3, 4, 1, 0, 5, 6, 7, 8};


struct animation_t {
    uint16_t timer;
    int8_t servos[NUMBER_OF_SERVOS];
};

struct MotionState {
    float curpos[kNumberOfJoints] = { 248, 560, 140, 475, 270, 250, 290, 375, 375, 180, 180};
    float setpos[kNumberOfJoints] = { 248, 560, 140, 475, 270, 250, 290, 375, 375,   0,   0};
    float curvel[kNumberOfJoints] = {   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0};
    float maxvel[kNumberOfJoints] = { 500, 400, 500,2400,2400, 600, 600,1200,1200, 255, 255};
    float accell[kNumberOfJoints] = { 350, 300, 480,1800,1800, 500, 500, 900, 900, 800, 800};

    int move_value = 0;
    int turn_value = 0;
    int turn_offset = 0;
    int motor_deadzone = 0;
    int pwmspeed = 255;

    bool auto_mode = false;
    int64_t anime_timer_us = 0;
    int64_t motor_idle_us = 0;        // last time a servo was moving
    int64_t move_until_us = 0;        // timed move (MCP tool), 0 = none
};


static Pca9685 pwm;
static MotionState state;
static QueueHandle_t anime_queue = nullptr;
static SemaphoreHandle_t state_lock = nullptr;
static adc_oneshot_unit_handle_t adc_handle = nullptr;
static int battery_level_ = -999;
static int light_level_ = 0;
static int64_t status_timer_us = 0;


WalleMotion& WalleMotion::GetInstance() {
    static WalleMotion instance;
    return instance;
}

int WalleMotion::battery_level() const {
    return battery_level_;
}

int WalleMotion::light_level() const {
    return light_level_;
}

bool WalleMotion::auto_mode() const {
    return state.auto_mode;
}


// -------------------------------------------------------------------
/// Motor driver (TB6612FNG via LEDC)
// -------------------------------------------------------------------

static void MotorsInit() {
    // Direction + standby pins (STBY is tied to 5V on this hardware -
    // the driver is always enabled and the firmware does not drive it)
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << MOTOR_AIN1_GPIO) | (1ULL << MOTOR_AIN2_GPIO) |
                       (1ULL << MOTOR_BIN1_GPIO) | (1ULL << MOTOR_BIN2_GPIO);
#if MOTOR_STBY_WIRED
    cfg.pin_bit_mask |= (1ULL << MOTOR_STBY_GPIO);
#endif
    cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);
#if MOTOR_STBY_WIRED
    gpio_set_level(MOTOR_STBY_GPIO, 1);     // leave standby
#endif

    // PWM timer: 20kHz 8-bit (inaudible)
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = MOTOR_PWM_BITS;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = MOTOR_PWM_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {};
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;
    ch.hpoint = 0;

    ch.channel = LEDC_CHANNEL_0;
    ch.gpio_num = MOTOR_PWMA_GPIO;
    ledc_channel_config(&ch);

    ch.channel = LEDC_CHANNEL_1;
    ch.gpio_num = MOTOR_PWMB_GPIO;
    ledc_channel_config(&ch);
}

static void MotorSetSpeed(ledc_channel_t channel, gpio_num_t dir1, gpio_num_t dir2, int pwm_value) {
    if (pwm_value > 255) pwm_value = 255;
    if (pwm_value < -255) pwm_value = -255;

    if (pwm_value > 0) {
        gpio_set_level(dir1, 1);
        gpio_set_level(dir2, 0);
    } else if (pwm_value < 0) {
        gpio_set_level(dir1, 0);
        gpio_set_level(dir2, 1);
    } else {
        // Short brake
        gpio_set_level(dir1, 1);
        gpio_set_level(dir2, 1);
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, (uint32_t)(pwm_value >= 0 ? pwm_value : -pwm_value));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}


// -------------------------------------------------------------------
/// Battery (ADC oneshot, linear scaling)
// -------------------------------------------------------------------

static void BatteryInit() {
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = BATTERY_ADC_UNIT;
    adc_oneshot_new_unit(&unit_cfg, &adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;       // ~3.1V full scale
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
}

void WalleMotion::CheckBattery() {
    int raw = 0;
    if (adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) return;

    float voltage = (raw * 3.1f / 4095.0f) / BATTERY_DIVIDER;
    int percentage = (int)(100 * (voltage - BATTERY_MIN_VOLTAGE) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE));
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;
    battery_level_ = percentage;
    ESP_LOGD(TAG, "Battery: %d%% (raw %d)", percentage, raw);
}


// -------------------------------------------------------------------
/// Command dispatcher (shared by MCP tools and the serial protocol)
// -------------------------------------------------------------------

void WalleMotion::EvaluateCommand(char prefix, int number) {

    ESP_LOGI(TAG, "Command %c%d", prefix, number);
    xSemaphoreTake(state_lock, portMAX_DELAY);

    // Motor inputs and offsets
    if      (prefix == 'X' && number >= -100 && number <= 100) state.turn_value = (int)(number * 2.55f);
    else if (prefix == 'Y' && number >= -100 && number <= 100) { state.move_value = (int)(number * 2.55f); state.move_until_us = 0; }
    else if (prefix == 'S' && number >= -100 && number <= 100) state.turn_offset = number;
    else if (prefix == 'O' && number >=    0 && number <= 250) state.motor_deadzone = number;

    // Animations
    else if (prefix == 'A') PlayAnimation(number);

    // Autonomous servo mode
    else if (prefix == 'M' && number == 0) state.auto_mode = false;
    else if (prefix == 'M' && number == 1) state.auto_mode = true;

    // Manual servo control (0..100 -> preset range)
    else if (prefix == 'L' && number >= 0 && number <= 100) {   // Left arm
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[5] = (int)(number * 0.01f * (preset[5][1] - preset[5][0]) + preset[5][0]);
    } else if (prefix == 'R' && number >= 0 && number <= 100) { // Right arm
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[6] = (int)(number * 0.01f * (preset[6][1] - preset[6][0]) + preset[6][0]);
    } else if (prefix == 'B' && number >= 0 && number <= 100) { // Neck bottom
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[2] = (int)(number * 0.01f * (preset[2][1] - preset[2][0]) + preset[2][0]);
    } else if (prefix == 'T' && number >= 0 && number <= 100) { // Neck top
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[1] = (int)(number * 0.01f * (preset[1][1] - preset[1][0]) + preset[1][0]);
    } else if (prefix == 'G' && number >= 0 && number <= 100) { // Head rotation
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[0] = (int)(number * 0.01f * (preset[0][1] - preset[0][0]) + preset[0][0]);
    } else if (prefix == 'E' && number >= 0 && number <= 100) { // Eye left
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[4] = (int)(number * 0.01f * (preset[4][1] - preset[4][0]) + preset[4][0]);
    } else if (prefix == 'U' && number >= 0 && number <= 100) { // Eye right
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[3] = (int)(number * 0.01f * (preset[3][1] - preset[3][0]) + preset[3][0]);
    } else if (prefix == 'I' && number >= 0 && number <= 100) { // Eyebrow left
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[7] = (int)(number * 0.01f * (preset[7][1] - preset[7][0]) + preset[7][0]);
    } else if (prefix == 'J' && number >= 0 && number <= 100) { // Eyebrow right
        state.auto_mode = false; xQueueReset(anime_queue);
        state.setpos[8] = (int)(number * 0.01f * (preset[8][1] - preset[8][0]) + preset[8][0]);
    } else if (prefix == 'V' && number >= 0 && number <= 100) { // Illumination LED brightness
        light_level_ = number;
        if (number == 0) pwm.SetFullOff(LIGHT_PWM_CHANNEL);
        else pwm.SetPwm(LIGHT_PWM_CHANNEL, (uint16_t)(number * 4095 / 100));
    }

    // WASD manual movements
    else if (prefix == 'w') {       // Forward
        state.move_value = state.pwmspeed; state.turn_value = 0; state.move_until_us = 0;
        state.setpos[0] = (preset[0][1] + preset[0][0]) / 2;
    }
    else if (prefix == 'q') {       // Stop
        state.move_value = 0; state.turn_value = 0; state.move_until_us = 0;
        state.setpos[0] = (preset[0][1] + preset[0][0]) / 2;
    }
    else if (prefix == 's') {       // Backward
        state.move_value = -state.pwmspeed; state.turn_value = 0; state.move_until_us = 0;
        state.setpos[0] = (preset[0][1] + preset[0][0]) / 2;
    }
    else if (prefix == 'a') {       // Drive & look left
        state.move_value = 0; state.turn_value = -state.pwmspeed; state.move_until_us = 0;
        state.setpos[0] = preset[0][0];
    }
    else if (prefix == 'd') {       // Drive & look right
        state.move_value = 0; state.turn_value = state.pwmspeed; state.move_until_us = 0;
        state.setpos[0] = preset[0][1];
    }

    // Eye expressions
    else if (prefix == 'j') {       // Left head tilt
        state.setpos[4] = preset[4][0];
        state.setpos[3] = preset[3][1];
    }
    else if (prefix == 'l') {       // Right head tilt
        state.setpos[4] = preset[4][1];
        state.setpos[3] = preset[3][0];
    }
    else if (prefix == 'i') {       // Sad head
        state.setpos[4] = preset[4][0];
        state.setpos[3] = preset[3][0];
    }
    else if (prefix == 'k') {       // Neutral head
        state.setpos[4] = (int)(0.4f * (preset[4][1] - preset[4][0]) + preset[4][0]);
        state.setpos[3] = (int)(0.4f * (preset[3][1] - preset[3][0]) + preset[3][0]);
    }

    // Head movement
    else if (prefix == 'f') {       // Head up
        state.setpos[1] = preset[1][0];
        state.setpos[2] = (preset[2][1] + preset[2][0]) / 2;
    }
    else if (prefix == 'g') {       // Head forward
        state.setpos[1] = preset[1][1];
        state.setpos[2] = preset[2][0];
    }
    else if (prefix == 'h') {       // Head down
        state.setpos[1] = preset[1][0];
        state.setpos[2] = preset[2][0];
    }

    // Arm movements
    else if (prefix == 'b') {       // Left arm low, right arm high
        state.setpos[5] = preset[5][0];
        state.setpos[6] = preset[6][1];
    }
    else if (prefix == 'n') {       // Both arms neutral
        state.setpos[5] = (preset[5][0] + preset[5][1]) / 2;
        state.setpos[6] = (preset[6][0] + preset[6][1]) / 2;
    }
    else if (prefix == 'm') {       // Left arm high, right arm low
        state.setpos[5] = preset[5][1];
        state.setpos[6] = preset[6][0];
    }

    xSemaphoreGive(state_lock);
}


// -------------------------------------------------------------------
/// Animations (data identical to animations.ino)
// -------------------------------------------------------------------

static void QueueAnim(uint16_t timer, int8_t s0, int8_t s1, int8_t s2, int8_t s3, int8_t s4,
                      int8_t s5, int8_t s6, int8_t s7, int8_t s8) {
    animation_t item = { timer, { s0, s1, s2, s3, s4, s5, s6, s7, s8 } };
    xQueueSend(anime_queue, &item, 0);
}

void WalleMotion::PlayAnimation(int animation_no) {
    switch (animation_no) {
        case 0:
            // --- Reset servo positions ---
            //       time,head,necT,necB,eyeR,eyeL,armL,armR,broL,broR
            QueueAnim(1000,  50,  10,   0,   0,   0,  40,  40,  50,  50);
            break;

        case 1:
            // --- Bootup eye sequence ---
            QueueAnim(2000,  50,  45,  90,  40,  40,  40,  40,  50,  50);
            QueueAnim( 700,  50,  45,  90,  40,   0,  40,  40,  50,  50);
            QueueAnim( 700,  50,  45,  90,   0,   0,  40,  40,  50,  50);
            QueueAnim( 700,  50,  45,  90,   0,  40,  40,  40,  50,  50);
            QueueAnim( 700,  50,  45,  90,  40,  40,  40,  40,  50,  50);
            QueueAnim( 400,  50,  45,  90,   0,   0,  40,  40,  50,  50);
            QueueAnim( 400,  50,  45,  90,  40,  40,  40,  40,  50,  50);
            QueueAnim(2000,  50,   0,  60,  40,  40,  40,  40,  50,  50);
            QueueAnim(1000,  50,   0,  60,   0,   0,  40,  40,  50,  50);
            break;

        case 2:
            // --- Inquisitive motion sequence ---
            QueueAnim(3000,  48,  40,   0,  35,  45,  60,  59,  50,  50);
            QueueAnim(1500,  48,  40,  20, 100,   0,  80,  80,  50,  50);
            QueueAnim(3000,   0,  40,  40, 100,   0,  80,  80,  50,  50);
            QueueAnim(1500,  48,  60, 100,  40,  40, 100, 100,  50,  50);
            QueueAnim(1500,  48,  40,  30,  45,  35,   0,   0,  50,  50);
            QueueAnim(1500,  34,  34,  10,  14, 100,   0,   0,  50,  50);
            QueueAnim(1500,  48,  60,  20,  35,  45,  60,  59,  50,  50);
            QueueAnim(3000, 100,  20,  50,  40,  40,  60, 100,  50,  50);
            QueueAnim(1500,  48,  15,   0,   0,   0,   0,   0,  50,  50);
            QueueAnim(1000,  50,  10,   0,   0,   0,  40,  40,  50,  50);
            break;

        default:
            ESP_LOGW(TAG, "Invalid animation %d requested", animation_no);
            break;
    }
}

void WalleMotion::ManageAnimations() {
    int64_t now = esp_timer_get_time();

    // Run queued animation waypoints
    if (uxQueueMessagesWaiting(anime_queue) > 0 && state.anime_timer_us <= now) {
        animation_t item;
        if (xQueueReceive(anime_queue, &item, 0) == pdTRUE) {
            state.anime_timer_us = now + (int64_t)item.timer * 1000;
            for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
                state.setpos[i] = (int)(item.servos[i] * 0.01f * (preset[i][1] - preset[i][0]) + preset[i][0]);
            }
        }
        return;
    }

    // Autonomous mode: generate random movements when idle
    if (state.auto_mode && uxQueueMessagesWaiting(anime_queue) == 0 && state.anime_timer_us <= now) {
        for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
            if ((esp_random() % 2) == 1) {
                if (i == 0 || i == 1 || i == 5 || i == 6) {
                    unsigned int min = preset[i][0], max = preset[i][1];
                    if (min > max) { min = max; max = preset[i][0]; }
                    state.setpos[i] = min + (esp_random() % (max - min + 1));
                } else if (i == 3) {
                    int mid1 = (int)((preset[i][1] - preset[i][0]) * 0.4f + preset[i][0]);
                    int mid2 = (int)((preset[i+1][1] - preset[i+1][0]) * 0.4f + preset[i+1][0]);

                    if ((esp_random() % 2) == 1) {
                        // Both eyes move downwards together
                        int lo = mid1 < preset[i][0] ? mid1 : preset[i][0];
                        int hi = mid1 < preset[i][0] ? preset[i][0] : mid1;
                        state.setpos[i] = lo + (esp_random() % (hi - lo + 1));
                        float mult = (state.setpos[i] - mid1) / (float)(preset[i][0] - mid1);
                        state.setpos[i+1] = ((1 - mult) * (mid2 - preset[i+1][0])) + preset[i+1][0];
                    } else {
                        // Eyes move in opposite directions
                        int lo = mid1 < preset[i][0] ? mid1 : preset[i][0];
                        int hi = mid1 < preset[i][0] ? preset[i][0] : mid1;
                        state.setpos[i] = lo + (esp_random() % (hi - lo + 1));
                        float mult = (state.setpos[i] - preset[i][1]) / (float)(preset[i][0] - preset[i][1]);
                        state.setpos[i+1] = (mult * (preset[i+1][1] - preset[i+1][0])) + preset[i+1][0];
                    }
                }
            }
        }
        state.anime_timer_us = now + (int64_t)(500 + esp_random() % 2500) * 1000;
    }
}


// -------------------------------------------------------------------
/// Servo dynamics (trapezoidal velocity, identical to Arduino)
// -------------------------------------------------------------------

void WalleMotion::ManageServos(float dt_ms) {
    bool moving = false;

    for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
        float pos_error = state.setpos[i] - state.curpos[i];

        if (std::fabs(pos_error) > CONTROLLER_THRESHOLD && state.setpos[i] != -1) {
            pwm.SetOutputEnable(false);     // outputs on
            moving = true;

            bool dir = pos_error > 0;

            float acceleration = state.accell[i];
            if ((state.curvel[i] * state.curvel[i] / (2 * state.accell[i])) > std::fabs(pos_error)) {
                acceleration = -state.accell[i];
            }

            if (dir) state.curvel[i] += acceleration * dt_ms / 1000.0f;
            else     state.curvel[i] -= acceleration * dt_ms / 1000.0f;

            if (state.curvel[i] > state.maxvel[i]) state.curvel[i] = state.maxvel[i];
            if (state.curvel[i] < -state.maxvel[i]) state.curvel[i] = -state.maxvel[i];

            float dp = state.curvel[i] * dt_ms / 1000.0f;
            if (std::fabs(dp) < std::fabs(pos_error)) state.curpos[i] += dp;
            else state.curpos[i] = state.setpos[i];

            pwm.SetPwm(servo_channel[i], (uint16_t)state.curpos[i]);
        } else {
            state.curvel[i] = 0;
        }
    }

    // Power-saving: turn servo outputs off after SERVO_OFF_MS idle
    if (moving) {
        state.motor_idle_us = esp_timer_get_time();
    } else if (esp_timer_get_time() - state.motor_idle_us >= (int64_t)SERVO_OFF_MS * 1000) {
        for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
            pwm.SetFullOff(servo_channel[i]);
        }
    }
}


// -------------------------------------------------------------------
/// Motor ramping (identical to Arduino manageMotors)
// -------------------------------------------------------------------

void WalleMotion::ManageMotors(float dt_ms) {

    state.setpos[NUMBER_OF_SERVOS] = state.move_value - state.turn_value;
    state.setpos[NUMBER_OF_SERVOS + 1] = state.move_value + state.turn_value;

    // Steering trim, only while driving
    if (state.setpos[NUMBER_OF_SERVOS] != 0) state.setpos[NUMBER_OF_SERVOS] -= state.turn_offset;
    if (state.setpos[NUMBER_OF_SERVOS + 1] != 0) state.setpos[NUMBER_OF_SERVOS + 1] += state.turn_offset;

    for (int i = NUMBER_OF_SERVOS; i < NUMBER_OF_SERVOS + 2; i++) {
        float vel_error = state.setpos[i] - state.curvel[i];

        if (std::fabs(vel_error) > CONTROLLER_THRESHOLD && state.setpos[i] != -1) {
            float acceleration = state.accell[i];
            if (state.setpos[i] < state.curvel[i] && state.curvel[i] >= 0) acceleration = -state.accell[i];
            else if (state.setpos[i] < state.curvel[i] && state.curvel[i] < 0) acceleration = -state.accell[i];
            else if (state.setpos[i] > state.curvel[i] && state.curvel[i] < 0) acceleration = state.accell[i];

            float dv = acceleration * dt_ms / 1000.0f;
            if (std::fabs(dv) < std::fabs(vel_error)) state.curvel[i] += dv;
            else state.curvel[i] = state.setpos[i];
        } else {
            state.curvel[i] = state.setpos[i];
        }

        // Deadzone compensation
        if (state.curvel[i] > 0) state.curvel[i] += state.motor_deadzone;
        else if (state.curvel[i] < 0) state.curvel[i] -= state.motor_deadzone;

        if (state.curvel[i] > state.maxvel[i]) state.curvel[i] = state.maxvel[i];
        if (state.curvel[i] < -state.maxvel[i]) state.curvel[i] = -state.maxvel[i];
    }

    MotorSetSpeed(LEDC_CHANNEL_0, MOTOR_AIN1_GPIO, MOTOR_AIN2_GPIO, (int)state.curvel[NUMBER_OF_SERVOS]);
    MotorSetSpeed(LEDC_CHANNEL_1, MOTOR_BIN1_GPIO, MOTOR_BIN2_GPIO, (int)state.curvel[NUMBER_OF_SERVOS + 1]);
}


// -------------------------------------------------------------------
/// Timed move helper (MCP move tool with duration)
// -------------------------------------------------------------------

void WalleMotion::StartTimedMove(int move, int turn, int duration_ms) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    state.move_value = (int)(move * 2.55f);
    state.turn_value = (int)(turn * 2.55f);
    state.move_until_us = duration_ms > 0 ? esp_timer_get_time() + (int64_t)duration_ms * 1000 : 0;
    xSemaphoreGive(state_lock);
}

void WalleMotion::UpdateMoveTimer() {
    if (state.move_until_us != 0 && esp_timer_get_time() >= state.move_until_us) {
        state.move_value = 0;
        state.turn_value = 0;
        state.move_until_us = 0;
    }
}


// -------------------------------------------------------------------
/// Servo soft start (identical to the Arduino version)
// -------------------------------------------------------------------

void WalleMotion::SoftStart() {
    // Target: animation 0 first waypoint
    static const int8_t target[NUMBER_OF_SERVOS] = { 50, 10, 0, 0, 0, 40, 40, 50, 50 };

    pwm.SetOutputEnable(false);
    for (int i = 0; i < NUMBER_OF_SERVOS; i++) {
        if (target[i] >= 0) {
            state.curpos[i] = (int)(target[i] * 0.01f * (preset[i][1] - preset[i][0]) + preset[i][0]);

            int64_t end = esp_timer_get_time() + (3500 / NUMBER_OF_SERVOS) * 1000;
            while (esp_timer_get_time() < end) {
                pwm.SetPwm(servo_channel[i], (uint16_t)state.curpos[i]);
                vTaskDelay(pdMS_TO_TICKS(10));
                pwm.SetFullOff(servo_channel[i]);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            pwm.SetPwm(servo_channel[i], (uint16_t)state.curpos[i]);
            state.setpos[i] = state.curpos[i];
        }
    }
}


// -------------------------------------------------------------------
/// Motion control task (100Hz)
// -------------------------------------------------------------------

void WalleMotion::MotionTaskEntry(void* arg) {
    ((WalleMotion*)arg)->MotionTask();
    vTaskDelete(nullptr);
}

void WalleMotion::MotionTask() {
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_time = esp_timer_get_time();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SERVO_UPDATE_MS));

        int64_t now = esp_timer_get_time();
        float dt = (now - last_time) / 1000.0f;      // microseconds -> ms
        last_time = now;

        xSemaphoreTake(state_lock, portMAX_DELAY);
        UpdateMoveTimer();
        ManageAnimations();
        ManageServos(dt);
        ManageMotors(dt);
        xSemaphoreGive(state_lock);

        if (now - status_timer_us >= (int64_t)STATUS_CHECK_MS * 1000) {
            status_timer_us = now;
            CheckBattery();
        }
    }
}


// -------------------------------------------------------------------
/// Initialisation
// -------------------------------------------------------------------

esp_err_t WalleMotion::Init() {

    state_lock = xSemaphoreCreateMutex();
    anime_queue = xQueueCreate(40, sizeof(animation_t));
    if (!state_lock || !anime_queue) return ESP_ERR_NO_MEM;

    esp_err_t ret = pwm.Init(SERVO_I2C_SDA_GPIO, SERVO_I2C_SCL_GPIO, SERVO_OE_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Servo PWM driver init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    pwm.SetFullOff(LIGHT_PWM_CHANNEL);      // illumination LED default off

    MotorsInit();
    BatteryInit();

    // Soft start the servos to their reset pose
    SoftStart();

    // Start the motion control task (priority 5: below audio/network)
    if (xTaskCreate(MotionTaskEntry, "walle_motion", 4096, this, 5, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Motion core started");
    return ESP_OK;
}
