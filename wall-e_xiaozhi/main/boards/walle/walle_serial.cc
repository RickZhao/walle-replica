/**
 * WALL-E SERIAL PROTOCOL TASK
 *
 * @file    walle_serial.cc
 * @brief   USB serial command interface, identical framing to the
 *          Arduino firmware (docs/SERIAL_PROTOCOL.md): 1 prefix char +
 *          up to 4 digits, terminated by \n or \r, max 5 chars.
 *          Keeps the Raspberry Pi fallback path (web_interface) and
 *          serial-monitor debugging working on the IDF firmware.
 *
 * Reads stdin (USB CDC / USB-Serial-JTAG console) in a dedicated task
 * and dispatches through WalleMotion::EvaluateCommand().
 */

#include "walle_motion.h"

#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define MAX_SERIAL_LENGTH 5


static void SerialTask(void*) {
    char first_char = 0;
    char buffer[MAX_SERIAL_LENGTH];
    uint8_t length = 0;

    while (true) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        char inchar = (char)c;

        // Frame end: evaluate the buffer
        if (inchar == '\n' || inchar == '\r') {
            if (length > 0 && first_char != 0) {
                WalleMotion::GetInstance().EvaluateCommand(first_char, atoi(buffer));
            }
            buffer[0] = 0;
            length = 0;
            first_char = 0;
            continue;
        }

        // Buffer the character
        if (length == 0) {
            first_char = inchar;
        } else {
            buffer[length - 1] = inchar;
            buffer[length] = 0;
        }
        length++;

        // Prevent overflow: evaluate when the buffer is full
        if (length == MAX_SERIAL_LENGTH) {
            if (first_char != 0) {
                WalleMotion::GetInstance().EvaluateCommand(first_char, atoi(buffer));
            }
            buffer[0] = 0;
            length = 0;
            first_char = 0;
        }
    }
}


void WalleSerialStart() {
    xTaskCreate(SerialTask, "walle_serial", 2048, nullptr, 3, nullptr);
}
