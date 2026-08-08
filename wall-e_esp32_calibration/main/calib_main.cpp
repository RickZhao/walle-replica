/**
 * WALL-E SERVO CALIBRATION — ESP-IDF port
 *
 * @file    calib_main.cpp
 * @brief   Calibrate 9 servo joints' PWM min/max positions via
 *          serial console. Outputs a ready-to-paste preset array
 *          for walle_motion.cc.
 *
 * How to use:
 *   1. Build & flash:  idf.py flash monitor
 *   2. Follow the on-screen prompts to calibrate each servo.
 *   3. Copy the printed preset[][] array into walle_motion.cc.
 *
 * Keys:  a/d = coarse (±10 PWM), z/c = fine (±1 PWM), n = confirm
 *
 * Ported from wall-e_esp32_calibration.ino — behaviour unchanged.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

// ====================================================================
// Hardware constants
// ====================================================================

#define SR_OE         GPIO_NUM_10   // servo driver output enable
#define I2C_SDA       GPIO_NUM_8
#define I2C_SCL       GPIO_NUM_9
#define PCA9685_ADDR   0x40
#define SERVOS         9

// PCA9685 registers
#define PCA_MODE1      0x00
#define PCA_PRESCALE   0xFE
#define PCA_LED0_ON_L  0x06
#define PCA_ALL_LED_ON 0xFA

// PWM channel for each logical joint
// Order: head, necT, necB, eyeR, eyeL, armL, armR, broL, broR
static uint8_t servoChannel[SERVOS] = {2, 3, 4, 1, 0, 5, 6, 7, 8};

// ====================================================================
// PCA9685 driver (inline, no external library)
// ====================================================================

static i2c_master_dev_handle_t pca_handle;

static void pcaWriteReg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_master_transmit(pca_handle, buf, 2, -1);
}

static void pcaInit() {
    // I2C bus config
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = I2C_SDA;
    bus_cfg.scl_io_num = I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = PCA9685_ADDR;
    dev_cfg.scl_speed_hz = 100000;

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &pca_handle));

    // Reset PCA9685
    pcaWriteReg(PCA_MODE1, 0x00);

    // Set PWM frequency = 60 Hz
    // freq = 25MHz / (4096 * prescaler) → prescaler ≈ 101
    uint8_t old_mode;
    i2c_master_transmit(pca_handle, (uint8_t[]){PCA_MODE1}, 1, -1);
    i2c_master_receive(pca_handle, &old_mode, 1, -1);
    uint8_t sleep_mode = (old_mode & 0x7F) | 0x10; // sleep bit
    pcaWriteReg(PCA_MODE1, sleep_mode);
    pcaWriteReg(PCA_PRESCALE, 101);
    pcaWriteReg(PCA_MODE1, old_mode);
    vTaskDelay(pdMS_TO_TICKS(5));
    pcaWriteReg(PCA_MODE1, old_mode | 0x80); // auto-increment + restart
}

/// Set PWM channel: on=0, off=pulse_width (4096 = full on)
static void pcaSetPWM(uint8_t channel, uint16_t off) {
    uint8_t reg = PCA_LED0_ON_L + 4 * channel;
    uint8_t buf[5] = {reg, 0, 0, (uint8_t)(off & 0xFF), (uint8_t)(off >> 8)};
    i2c_master_transmit(pca_handle, buf, 5, -1);
}

/// Disable a channel (off = 0)
static void pcaSetPinOff(uint8_t channel) {
    pcaSetPWM(channel, 0);
}

// ====================================================================
// Calibration state
// ====================================================================

// Default preset values (seeded from the Arduino sketch)
static int preset[SERVOS][2] = {
    {398, 112},  // head rotation
    {565, 188},  // neck top
    {200, 400},  // neck bottom
    {475, 230},  // eye right
    {270, 440},  // eye left
    {350, 185},  // arm left
    {188, 360},  // arm right
    {150, 600},  // eyebrow left
    {150, 600},  // eyebrow right
};

static float restpos[SERVOS] = {50, 50, 40, 0, 0, 100, 100, 50, 50};

static const char *jointNames[SERVOS] = {
    "Head Rotation", "Neck Top Joint", "Neck Bottom Joint",
    "Eye Right", "Eye Left", "Arm Left", "Arm Right",
    "Eyebrow Left", "Eyebrow Right",
};

static const char *posNames[SERVOS][2] = {
    {"LOW (head facing left)",   "HIGH (head facing right)"},
    {"LOW (head looking up)",    "HIGH (head looking down)"},
    {"LOW (head looking down)",  "HIGH (head looking up)"},
    {"LOW (eye rotated down)",   "HIGH (eye rotated up)"},
    {"LOW (eye rotated down)",   "HIGH (eye rotated up)"},
    {"LOW (arm rotated down)",   "HIGH (arm rotated up)"},
    {"LOW (arm rotated down)",   "HIGH (arm rotated up)"},
    {"LOW (eyebrow lowered)",    "HIGH (eyebrow raised)"},
    {"LOW (eyebrow lowered)",    "HIGH (eyebrow raised)"},
};

static int currentServo = 0;
static int currentPosition = -1;
static int position;

// ====================================================================
// Helpers
// ====================================================================

static int64_t millis() {
    return esp_timer_get_time() / 1000;
}

static void changeServoPosition(int newPosition) {
    while (position != newPosition) {
        if (position < newPosition) position++;
        else position--;
        pcaSetPWM(servoChannel[currentServo], (uint16_t)position);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void softStart() {
    int64_t endTime = millis() + 1000;
    while (millis() < endTime) {
        pcaSetPWM(servoChannel[currentServo], (uint16_t)position);
        vTaskDelay(pdMS_TO_TICKS(10));
        pcaSetPinOff(servoChannel[currentServo]);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    pcaSetPWM(servoChannel[currentServo], (uint16_t)position);
}

static void moveToNextPosition() {
    if (currentPosition != -1) {
        preset[currentServo][currentPosition] = position;
        printf("[Confirmed Position: %d]\n\n", position);
    }

    if (currentPosition < 1) {
        currentPosition++;
    } else {
        // Move servo to rest position before switching
        int rest = (int)(restpos[currentServo] / 100.0f *
                         (preset[currentServo][1] - preset[currentServo][0]) +
                         preset[currentServo][0]);
        changeServoPosition(rest);
        pcaSetPinOff(servoChannel[currentServo]);
        currentServo++;
        currentPosition = 0;
        position = preset[currentServo][currentPosition] - 1;

        if (currentServo == SERVOS) {
            gpio_set_level(SR_OE, 1);  // disable servo outputs
            // Output results
            printf("\nCalibrated values - copy and paste these into the "
                   "'preset' array in walle_motion.cc:\n\n");
            printf("int preset[][2] =  {{%d,%d},  // head rotation\n",
                   preset[0][0], preset[0][1]);
            printf("                    {%d,%d},  // neck top\n",
                   preset[1][0], preset[1][1]);
            printf("                    {%d,%d},  // neck bottom\n",
                   preset[2][0], preset[2][1]);
            printf("                    {%d,%d},  // eye right\n",
                   preset[3][0], preset[3][1]);
            printf("                    {%d,%d},  // eye left\n",
                   preset[4][0], preset[4][1]);
            printf("                    {%d,%d},  // arm left\n",
                   preset[5][0], preset[5][1]);
            printf("                    {%d,%d},  // arm right\n",
                   preset[6][0], preset[6][1]);
            printf("                    {%d,%d},  // eyebrow left\n",
                   preset[7][0], preset[7][1]);
            printf("                    {%d,%d}}; // eyebrow right\n",
                   preset[8][0], preset[8][1]);
            printf("\n--- Done. Halting. ---\n");
            while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
        }
        softStart();
    }

    printf("%s - %s\n", jointNames[currentServo], posNames[currentServo][currentPosition]);
    printf("-----------------------------------\n");
    printf("Keys: a/d=±10  z/c=±1  n=confirm position\n");

    changeServoPosition(preset[currentServo][currentPosition]);
}

static void processInput(char c) {
    if (c == 'n') {
        moveToNextPosition();
    } else if (c == 'a') {
        changeServoPosition(position - 10);
    } else if (c == 'd') {
        changeServoPosition(position + 10);
    } else if (c == 'z') {
        changeServoPosition(position - 1);
    } else if (c == 'c') {
        changeServoPosition(position + 1);
    }
}

// ====================================================================
// UART input task
// ====================================================================

static void uartTask(void *) {
    char buf[1];
    while (true) {
        int n = fread(buf, 1, 1, stdin);
        if (n == 1) processInput(buf[0]);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ====================================================================
// Main
// ====================================================================

extern "C" void app_main() {
    // OE pin
    gpio_config_t oe_cfg = {};
    oe_cfg.pin_bit_mask = (1ULL << SR_OE);
    oe_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&oe_cfg);
    gpio_set_level(SR_OE, 1);  // servos disabled until init done

    // I2C + PCA9685
    pcaInit();

    // Turn off all channels
    for (int i = 0; i < SERVOS; i++) pcaSetPinOff(i);

    // Wait for USB serial to connect (max 5s)
    int64_t waitStart = millis();
    while (millis() - waitStart < 5000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("\n////////// Starting Wall-E Calibration Program (ESP32-S3) //////////\n\n");

    // Enable servo outputs
    gpio_set_level(SR_OE, 0);

    position = preset[0][0] - 1;
    softStart();
    moveToNextPosition();

    // Start UART reader
    xTaskCreate(uartTask, "uart_in", 2048, nullptr, 5, nullptr);
}
