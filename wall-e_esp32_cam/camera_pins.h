/**
 * CAMERA MODULE PIN DEFINITIONS
 *
 * @file    camera_pins.h
 * @brief   Pin presets for common ESP32-S3 camera modules.
 *
 * The third-party BOM only says "ESP32 S3 cam module" - check which
 * board you actually have and select exactly ONE preset below.
 * If your module is not listed, copy a preset block and adjust the
 * pins according to the vendor's documentation.
 */

#ifndef CAMERA_PINS_H
#define CAMERA_PINS_H

// Select your camera module (exactly one):
#define CAM_BOARD_XIAO_ESP32S3_SENSE
// #define CAM_BOARD_FREENOVE_ESP32S3_WROOM
// #define CAM_BOARD_ESP32S3_EYE


#if defined(CAM_BOARD_XIAO_ESP32S3_SENSE)
	// Seeed XIAO ESP32-S3 Sense (OV2640 expansion board)
	#define PWDN_GPIO_NUM  -1
	#define RESET_GPIO_NUM -1
	#define XCLK_GPIO_NUM  10
	#define SIOD_GPIO_NUM  40
	#define SIOC_GPIO_NUM  39
	#define Y9_GPIO_NUM    48
	#define Y8_GPIO_NUM    11
	#define Y7_GPIO_NUM    12
	#define Y6_GPIO_NUM    14
	#define Y5_GPIO_NUM    16
	#define Y4_GPIO_NUM    18
	#define Y3_GPIO_NUM    17
	#define Y2_GPIO_NUM    15
	#define VSYNC_GPIO_NUM 38
	#define HREF_GPIO_NUM  47
	#define PCLK_GPIO_NUM  13

#elif defined(CAM_BOARD_FREENOVE_ESP32S3_WROOM)
	// Freenove ESP32-S3-WROOM (onboard OV2640)
	#define PWDN_GPIO_NUM  -1
	#define RESET_GPIO_NUM -1
	#define XCLK_GPIO_NUM  15
	#define SIOD_GPIO_NUM  4
	#define SIOC_GPIO_NUM  5
	#define Y9_GPIO_NUM    16
	#define Y8_GPIO_NUM    17
	#define Y7_GPIO_NUM    18
	#define Y6_GPIO_NUM    12
	#define Y5_GPIO_NUM    10
	#define Y4_GPIO_NUM    8
	#define Y3_GPIO_NUM    9
	#define Y2_GPIO_NUM    11
	#define VSYNC_GPIO_NUM 6
	#define HREF_GPIO_NUM  7
	#define PCLK_GPIO_NUM  13

#elif defined(CAM_BOARD_ESP32S3_EYE)
	// Espressif ESP32-S3-EYE
	#define PWDN_GPIO_NUM  -1
	#define RESET_GPIO_NUM -1
	#define XCLK_GPIO_NUM  15
	#define SIOD_GPIO_NUM  4
	#define SIOC_GPIO_NUM  5
	#define Y9_GPIO_NUM    16
	#define Y8_GPIO_NUM    17
	#define Y7_GPIO_NUM    18
	#define Y6_GPIO_NUM    12
	#define Y5_GPIO_NUM    10
	#define Y4_GPIO_NUM    8
	#define Y3_GPIO_NUM    9
	#define Y2_GPIO_NUM    11
	#define VSYNC_GPIO_NUM 6
	#define HREF_GPIO_NUM  7
	#define PCLK_GPIO_NUM  13

#else
	#error "No camera board selected - edit camera_pins.h"
#endif

#endif /* CAMERA_PINS_H */
