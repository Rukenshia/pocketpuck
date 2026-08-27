#pragma once

#include <Arduino.h>

// Use the labels printed on the Nano ESP32, not the ESP32-S3 GPIO numbers.
// The display is write-only, so reuse D12 (normally CIPO) for its clock.
// D13 is deliberately avoided because it also drives the built-in yellow LED.
constexpr uint8_t LCD_COPI = D11;
constexpr uint8_t LCD_SCK = D12;
constexpr uint8_t LCD_CS = D10;
constexpr uint8_t LCD_DC = D7;
constexpr uint8_t LCD_RST = D8;
constexpr uint8_t LCD_BL = D9;

// This resistor-only encoder module draws very little current, so D5 supplies
// its 3.3 V pull-up rail. Do not use this pin as a general-purpose power rail.
constexpr uint8_t ROTARY_VCC = D5;
constexpr uint8_t ROTARY_CLK = D2;
constexpr uint8_t ROTARY_DT = D3;
constexpr uint8_t ROTARY_SW = D4;

constexpr uint16_t LCD_NATIVE_WIDTH = 240;
constexpr uint16_t LCD_NATIVE_HEIGHT = 320;
constexpr uint8_t LCD_ROTATION = 1;  // 320 x 240 landscape
