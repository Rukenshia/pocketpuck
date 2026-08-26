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

## Live Amp stats

Amp's supported External API does not currently expose live thread execution
state. PocketPuck therefore uses a small bridge which runs the authenticated
Amp CLI and turns `amp top` into a local HTTP endpoint. The Amp API key remains
on the bridge host rather than being copied to the microcontroller.

On a Raspberry Pi or another always-on computer, install and log into Amp, then
run:

```sh
python3 scripts/pocketpuck_bridge.py
curl http://localhost:8765/stats
```

The response has this shape:

```json
{"running":1,"idle":11,"updatedAt":"2026-08-26T21:02:57.697Z","reconnecting":false}
```

`running` counts entries that `amp top` reports as working. `idle` counts the
remaining entries in its active-thread list; it is not a count of every
historical or archived thread.

Configure the firmware with the Pi's LAN address:

```sh
cp include/network_config.example.h include/network_config.h
```

Edit `include/network_config.h`, then build and upload normally. This local
file is ignored by git. The display retries Wi-Fi every 15 seconds and polls
the bridge every 10 seconds. Until configured or connected, it shows the
corresponding status instead of stale counts.

To run the bridge under systemd, use a service like this (adjust the user,
repository path, and Amp executable path):

```ini
[Unit]
Description=PocketPuck Amp bridge
After=network-online.target

[Service]
User=pi
WorkingDirectory=/home/pi/pocketpuck
ExecStart=/usr/bin/python3 scripts/pocketpuck_bridge.py --amp-command /home/pi/.local/bin/amp
Restart=always

[Install]
WantedBy=multi-user.target
```
