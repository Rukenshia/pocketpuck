# PocketPuck

A tiny display companion for [Amp](https://ampcode.com), built with an Arduino
Nano ESP32 and a Waveshare 2inch LCD Module. The first firmware is a display-only
personality reel for a puck-shaped enclosure: Amp's wordmark rolls through its
dark splash animation, then gives way to a living face.

The demo treats Puck as an ancient, slightly bewildered computational familiar:
capable, deadpan, and quietly surprised by success. It loops through a slow
wake-up, a long frowning idle, mild bewilderment, thinking, a restrained
reaction, and one dry observation. Everything except the compressed Amp
wordmark is drawn procedurally, so expressions can be changed without generating
new image assets.

## Hardware

- Arduino Nano ESP32
- [Waveshare 2inch LCD Module](https://www.waveshare.com/wiki/2inch_LCD_Module)
  (240 x 320, ST7789V, 4-wire SPI)
- 8 jumper wires

### Wiring

Disconnect USB power while wiring. Use the **Nano pin labels** in this table,
not the ESP32-S3's raw GPIO numbers.

| Waveshare LCD | Nano ESP32 | Purpose |
| --- | --- | --- |
| `VCC` | `3V3` | Power |
| `GND` | `GND` | Ground |
| `DIN` | `D11` / `COPI` | SPI data to display |
| `CLK` | `D13` / `SCK` | SPI clock |
| `CS` | `D10` | Chip select |
| `DC` | `D7` | Data/command select |
| `RST` | `D8` | Display reset |
| `BL` | `D9` | Backlight, active high |

The Nano ESP32 GPIOs use 3.3 V logic, so the display is powered from `3V3` to
keep its supply and logic voltages consistent. Do not connect a GPIO to 5 V.
`D12` / `CIPO` is unused because this display is write-only.

## Build and upload

[PlatformIO](https://platformio.org/) is the only required development tool.

```sh
pio run
pio run --target upload
pio device monitor
```

PlatformIO should automatically find the Nano ESP32's USB port. If an upload
cannot find the board, double-press RESET to enter the Arduino bootloader (the
green LED pulses), then run the upload command again.

The pin assignments live in [`include/display_config.h`](include/display_config.h).
The firmware uses the Adafruit GFX and ST7789 libraries, fetched automatically
by PlatformIO.

## Current scope

This demo cycles through expressions on a timer; it does not yet communicate
with Amp. A later iteration can map real Amp states to these expressions over
USB serial or Wi-Fi without changing the display wiring.
