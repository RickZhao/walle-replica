#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================================================
// Wall-E voice robot board (xiaozhi-esp32 port)
// ESP32-S3 N16R8, single-MCU design:
//   voice (INMP441 + PCM5102) + GC9A01 round eye display + TB6612 motors
//   + LU9685/PCA9685 servo driver (9x MG90S) + battery ADC
// Pin map follows wall-e_esp32/web_config.h where possible.
// ============================================================================

// ---- Audio: INMP441 mic + PCM5102 DAC on two separate I2S ports ----
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_40   // INMP441 SCK
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_41   // INMP441 WS
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_44   // INMP441 SD
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_12   // PCM5102 BCK
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_13   // PCM5102 LRCK
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_11   // PCM5102 DIN

// ---- Eye display: single GC9A01 1.28" round 240x240 (BOM item 14) ----
// NOTE: the BOM carries ONE display module plus two eye lenses; both
// eyes are rendered on the single screen (electron-bot style).
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY  false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_SPI_SCLK_PIN  GPIO_NUM_21
#define DISPLAY_SPI_MOSI_PIN  GPIO_NUM_18
#define DISPLAY_SPI_CS_PIN    GPIO_NUM_38
#define DISPLAY_SPI_DC_PIN    GPIO_NUM_39
#define DISPLAY_SPI_RESET_PIN GPIO_NUM_47
#define DISPLAY_SPI_SCLK_HZ   (40 * 1000 * 1000)

#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_48
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ---- Buttons (BOM: 4x CHA PB13; only BOOT is wired for now) ----
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// ---- Motion hardware (used by walle_motion, stage 2) ----
// TB6612FNG dual motor driver
#define MOTOR_AIN1_GPIO GPIO_NUM_5
#define MOTOR_AIN2_GPIO GPIO_NUM_6
#define MOTOR_BIN1_GPIO GPIO_NUM_15
#define MOTOR_BIN2_GPIO GPIO_NUM_16
#define MOTOR_PWMA_GPIO GPIO_NUM_4
#define MOTOR_PWMB_GPIO GPIO_NUM_7
#define MOTOR_STBY_GPIO GPIO_NUM_17

// LU9685/PCA9685 16-channel servo PWM board (I2C @ 0x40)
#define SERVO_I2C_SDA_GPIO GPIO_NUM_8
#define SERVO_I2C_SCL_GPIO GPIO_NUM_9
#define SERVO_OE_GPIO      GPIO_NUM_10

// Battery voltage divider (ADC1_CH0); divider must bring 12.6V <= ~3.1V
#define BATTERY_ADC_GPIO   GPIO_NUM_1
#define BATTERY_ADC_UNIT   ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0

// ---- Optional ST7789 status display (stage 3, shares the SPI bus) ----
// 1.3" 240x240, CS/DC below; SCK/MOSI/RST/BL shared with the GC9A01.
#define STATUS_DISPLAY_ENABLED  1
#define STATUS_SPI_CS_PIN       GPIO_NUM_42
#define STATUS_SPI_DC_PIN       GPIO_NUM_14
#define STATUS_DISPLAY_WIDTH    240
#define STATUS_DISPLAY_HEIGHT   240
#define STATUS_DISPLAY_MIRROR_X false
#define STATUS_DISPLAY_MIRROR_Y true
#define STATUS_DISPLAY_SWAP_XY  false
#define STATUS_DISPLAY_OFFSET_X 0
#define STATUS_DISPLAY_OFFSET_Y 0

// ---- Web control panel (stage 3, esp_http_server on port 80) ----
#define WEB_SERVER_ENABLED      1
#define WEB_SERVER_PORT         80

#endif // _BOARD_CONFIG_H_
