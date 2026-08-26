#pragma once

#include <Arduino.h>

// Use the labels printed on the Nano ESP32, not the ESP32-S3 GPIO numbers.
// D11 and D13 are the board's default hardware SPI pins.
constexpr uint8_t LCD_CS = D10;
constexpr uint8_t LCD_DC = D7;
constexpr uint8_t LCD_RST = D8;
constexpr uint8_t LCD_BL = D9;

constexpr uint16_t LCD_NATIVE_WIDTH = 240;
constexpr uint16_t LCD_NATIVE_HEIGHT = 320;
constexpr uint8_t LCD_ROTATION = 1;  // 320 x 240 landscape
