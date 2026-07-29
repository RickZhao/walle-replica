/**
 * Minimal PCA9685/LU9685 16-channel PWM servo driver (ESP-IDF)
 *
 * @file    pca9685.cc
 * @brief   Implementation, see pca9685.h
 */

#include "pca9685.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

#define TAG "Pca9685"

// PCA9685 registers
#define REG_MODE1        0x00
#define REG_MODE2        0x01
#define REG_LED0_ON_L    0x06
#define REG_PRESCALE     0xFE

#define MODE1_SLEEP      0x10
#define MODE1_AUTOINC    0x20
#define MODE1_RESTART    0x80
#define MODE2_OUTDRV     0x04

#define OSC_CLOCK_HZ     25000000.0f


esp_err_t Pca9685::Init(gpio_num_t sda, gpio_num_t scl, gpio_num_t oe_pin, uint8_t i2c_addr) {

    oe_pin_ = oe_pin;

    // OE pin (active low): start with outputs disabled
    if (oe_pin_ != GPIO_NUM_NC) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << oe_pin_;
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);
        gpio_set_level(oe_pin_, 1);
    }

    // I2C bus (the Wall-E board has no other I2C devices, use port 0)
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = (i2c_port_t)0;
    bus_cfg.sda_io_num = sda;
    bus_cfg.scl_io_num = scl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus_);
    if (ret != ESP_OK) return ret;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = i2c_addr;
    dev_cfg.scl_speed_hz = 400 * 1000;
    ret = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (ret != ESP_OK) return ret;

    // Reset into a known state, then set 60Hz
    // MODE2: totem-pole outputs (required for servo signal lines)
    ret = WriteReg(REG_MODE2, MODE2_OUTDRV);
    if (ret != ESP_OK) return ret;

    // PRESCALE can only be written while the oscillator is asleep
    uint8_t prescale = (uint8_t)std::lroundf(OSC_CLOCK_HZ / (4096.0f * 60.0f)) - 1;
    ret = WriteReg(REG_MODE1, MODE1_SLEEP);
    if (ret != ESP_OK) return ret;
    ret = WriteReg(REG_PRESCALE, prescale);
    if (ret != ESP_OK) return ret;

    // Wake up with auto-increment enabled (lets us write 4 regs in one go)
    ret = WriteReg(REG_MODE1, MODE1_AUTOINC);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(1));           // oscillator needs 500us to stabilise
    ret = WriteReg(REG_MODE1, MODE1_AUTOINC | MODE1_RESTART);
    if (ret != ESP_OK) return ret;

    // All channels off
    for (uint8_t ch = 0; ch < 16; ch++) {
        SetFullOff(ch);
    }

    initialized_ = true;
    ESP_LOGI(TAG, "PCA9685 initialised at 60Hz (prescale %u)", prescale);
    return ESP_OK;
}


esp_err_t Pca9685::SetPwm(uint8_t channel, uint16_t off) {
    if (!initialized_ || channel > 15) return ESP_ERR_INVALID_ARG;
    if (off > 4095) off = 4095;

    // ON = 0, OFF = off (4 registers, auto-increment)
    uint8_t data[4] = {
        0x00, 0x00,
        (uint8_t)(off & 0xFF), (uint8_t)(off >> 8),
    };
    return WriteRegs(REG_LED0_ON_L + 4 * channel, data, sizeof(data));
}


esp_err_t Pca9685::SetFullOff(uint8_t channel) {
    if (!initialized_ || channel > 15) return ESP_ERR_INVALID_ARG;

    // OFF_H bit 4 = output fully off (same as Adafruit setPin(ch, 0))
    uint8_t data[4] = { 0x00, 0x00, 0x00, 0x10 };
    return WriteRegs(REG_LED0_ON_L + 4 * channel, data, sizeof(data));
}


void Pca9685::SetOutputEnable(bool disabled) {
    if (oe_pin_ != GPIO_NUM_NC) {
        gpio_set_level(oe_pin_, disabled ? 1 : 0);
    }
}


esp_err_t Pca9685::WriteReg(uint8_t reg, uint8_t value) {
    return WriteRegs(reg, &value, 1);
}


esp_err_t Pca9685::WriteRegs(uint8_t reg, const uint8_t* data, size_t len) {
    // PCA9685 protocol: first byte = register address, then payload
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    for (size_t i = 0; i < len; i++) buf[i + 1] = data[i];
    return i2c_master_transmit(dev_, buf, len + 1, 100);
}
