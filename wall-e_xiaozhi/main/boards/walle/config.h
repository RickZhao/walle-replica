#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================================================
// Wall-E voice robot board (xiaozhi-esp32 port)
// ESP32-S3 N16R8, single-MCU design:
//   voice (INMP441 + PCM5102) + GC9A01 round eye display + TB6612 motors
//   + LU9685/PCA9685 servo driver (9x MG90S) + battery ADC
// Pin map follows hardware/另一套硬件方案.md (third-party Wall-E kit).
//
// WARNING: GPIO19/GPIO20 are the ESP32-S3 native USB D-/D+ pins and are
// used here by PWMA and servo-I2C SDA. USB-CDC serial (walle_serial) is
// therefore disabled; logs go to UART0 (GPIO43/44).
// ============================================================================

// ---- Audio: INMP441 mic + PCM5102 DAC on two separate I2S ports ----
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5    // INMP441 SCK
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4    // INMP441 WS
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6    // INMP441 SD
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15   // PCM5102 BCK
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16   // PCM5102 LRCK
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7    // PCM5102 DIN

// ---- Eye display: single GC9A01 1.28" round 240x240 (BOM item 14) ----
// NOTE: the BOM carries ONE display module plus two eye lenses; both
// eyes are rendered on the single screen (electron-bot style).
// TODO: eye-display wiring is missing from the third-party pin table and
// awaits user confirmation. The pin values below are the OLD wiring and
// CONFLICT with the new hardware (servo I2C, motors, volume buttons,
// ST7789) — the eye display is therefore DISABLED until the real wiring
// is known: set the pins and flip EYE_DISPLAY_ENABLED to 1.
#define EYE_DISPLAY_ENABLED 0

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY  false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_SPI_SCLK_PIN  GPIO_NUM_21   // TODO: conflicts with servo I2C SCL
#define DISPLAY_SPI_MOSI_PIN  GPIO_NUM_18   // TODO: conflicts with motor AIN2
#define DISPLAY_SPI_CS_PIN    GPIO_NUM_38   // TODO: conflicts with volume-up button
#define DISPLAY_SPI_DC_PIN    GPIO_NUM_39   // TODO: conflicts with volume-down button
#define DISPLAY_SPI_RESET_PIN GPIO_NUM_47   // TODO: conflicts with ST7789 MOSI
#define DISPLAY_SPI_SCLK_HZ   (40 * 1000 * 1000)

#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_48  // TODO: unconfirmed
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ---- Buttons (BOM: 4x CHA PB13) ----
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_38
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39
#define RESET_BUTTON_GPIO       GPIO_NUM_41   // long-press system restart

// ---- Motion hardware (used by walle_motion) ----
// TB6612FNG dual motor driver. STBY is tied to 5V on the PCB (always
// enabled), so the firmware must not drive it.
#define MOTOR_AIN1_GPIO GPIO_NUM_17
#define MOTOR_AIN2_GPIO GPIO_NUM_18
#define MOTOR_BIN1_GPIO GPIO_NUM_12
#define MOTOR_BIN2_GPIO GPIO_NUM_13
#define MOTOR_PWMA_GPIO GPIO_NUM_19
#define MOTOR_PWMB_GPIO GPIO_NUM_11
#define MOTOR_STBY_GPIO GPIO_NUM_NC
// 0 = STBY tied to 5V on the PCB (driver always enabled, firmware must
// not drive it); 1 = STBY wired to MOTOR_STBY_GPIO.
#define MOTOR_STBY_WIRED 0

// LU9685/PCA9685 16-channel servo PWM board (I2C @ 0x40).
// OE is not wired on this hardware.
#define SERVO_I2C_SDA_GPIO GPIO_NUM_20
#define SERVO_I2C_SCL_GPIO GPIO_NUM_21
#define SERVO_OE_GPIO      GPIO_NUM_NC

// Illumination LED on a spare PCA9685 channel (12-bit PWM dimming).
// The channel header has V+/GND/signal; a small LED + series resistor can
// be driven directly (~10mA source / 25mA sink), a brighter lamp needs an
// N-MOSFET low-side switch on the signal pin.
#define LIGHT_PWM_CHANNEL    9

// Battery voltage divider (ADC1_CH0); divider must bring 12.6V <= ~3.1V.
// Not documented in the third-party pin table — GPIO1 is free there, but
// the divider wiring must be confirmed before enabling battery readings.
#define BATTERY_ADC_GPIO   GPIO_NUM_1
#define BATTERY_ADC_UNIT   ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0

// ---- ST7789 status display (BOM item 8: 1.3" 7-pin, 240x240) ----
// 7-pin module has NO CS pin (CS tied low internally) and sits on its own
// SPI bus, separate from the GC9A01 eye display.
#define STATUS_DISPLAY_ENABLED  1
#define STATUS_SPI_SCLK_PIN     GPIO_NUM_14
#define STATUS_SPI_MOSI_PIN     GPIO_NUM_47
#define STATUS_SPI_CS_PIN       GPIO_NUM_NC
#define STATUS_SPI_DC_PIN       GPIO_NUM_40
#define STATUS_SPI_RESET_PIN    GPIO_NUM_45
#define STATUS_SPI_BL_PIN       GPIO_NUM_42
#define STATUS_DISPLAY_WIDTH    240
#define STATUS_DISPLAY_HEIGHT   240
#define STATUS_DISPLAY_MIRROR_X false
#define STATUS_DISPLAY_MIRROR_Y true
#define STATUS_DISPLAY_SWAP_XY  false
#define STATUS_DISPLAY_OFFSET_X 0
#define STATUS_DISPLAY_OFFSET_Y 0

// ---- Web control panel (esp_http_server on port 80) ----
#define WEB_SERVER_ENABLED      1
#define WEB_SERVER_PORT         80

// ---- USB serial protocol task (Pi fallback + debug, walle_serial) ----
// DISABLED by default: this hardware uses GPIO19/GPIO20 (native USB D-/D+)
// for PWMA and servo-I2C SDA, so the USB-CDC console cannot be used.
// Logs go to UART0 (GPIO43/44).
#define WALLE_SERIAL_ENABLED    0

// ---- ESP32-S3-CAM module (wall-e_esp32_cam) ----
// Control + file transfer over UART (this hardware wires the CAM to
// GPIO9/10); Wi-Fi MJPEG preview (/stream) still uses CAM_MODULE_URL.
#define CAM_UART_TX_PIN  GPIO_NUM_9    // -> CAM GPIO14 (RX)
#define CAM_UART_RX_PIN  GPIO_NUM_10   // <- CAM GPIO21 (TX)
#define CAM_UART_BAUD    115200
// CAM Wi-Fi base URL for the MJPEG preview; find its IP in the module's
// serial output, e.g. "http://192.168.4.3".
#define CAM_MODULE_URL          "http://192.168.4.3"

// ---- 4G fallback: ML307A Cat.1 modem on UART (TX/RX from ESP32 view) ----
// Kept from the previous design: GPIO2/3 are free in the third-party pin
// table (the modem itself is not in the BOM — wire it if 4G is wanted).
#define ML307_TX_PIN   GPIO_NUM_2
#define ML307_RX_PIN   GPIO_NUM_3
#define ML307_DTR_PIN  GPIO_NUM_NC
// Wi-Fi connect timeout -> auto switch to 4G (1=on, 0=enter Wi-Fi config mode)
#define WIFI_AUTO_FALLBACK_4G 1

#endif // _BOARD_CONFIG_H_
