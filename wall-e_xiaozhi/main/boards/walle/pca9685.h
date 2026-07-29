/**
 * Minimal PCA9685/LU9685 16-channel PWM servo driver (ESP-IDF)
 *
 * @file    pca9685.h
 * @brief   Register-level I2C driver for the PCA9685-compatible servo
 *          PWM board (third-party BOM item 6, LU9685, default address
 *          0x40). Only implements what the Wall-E motion core needs:
 *          60Hz mode, setPWM and full-off per channel.
 *
 * Register reference: PCA9685 datasheet (NXP).
 */

#pragma once

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_err.h>

class Pca9685 {
public:
    Pca9685() = default;

    /// Initialise the I2C bus and the PCA9685 at 60Hz (analog servos).
    /// @param sda / scl   I2C pins
    /// @param oe_pin      Output-enable pin (active low), GPIO_NUM_NC if unused
    /// @param i2c_addr    Device address (default 0x40)
    esp_err_t Init(gpio_num_t sda, gpio_num_t scl, gpio_num_t oe_pin, uint8_t i2c_addr = 0x40);

    /// Set the PWM off-time of a channel (0-4095, on-time fixed at 0).
    esp_err_t SetPwm(uint8_t channel, uint16_t off);

    /// Turn a channel output fully off (used by the servo power-saving
    /// timeout - identical to Adafruit_PWMServoDriver::setPin(ch, 0)).
    esp_err_t SetFullOff(uint8_t channel);

    /// Drive the OE pin (false = outputs enabled).
    void SetOutputEnable(bool disabled);

private:
    esp_err_t WriteReg(uint8_t reg, uint8_t value);
    esp_err_t WriteRegs(uint8_t reg, const uint8_t* data, size_t len);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    gpio_num_t oe_pin_ = GPIO_NUM_NC;
    bool initialized_ = false;
};
