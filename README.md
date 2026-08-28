# PocketPuck

> [!NOTE]
> This project is not associated with Amp. It's just a fun little side project to put Puck on my desk.

|                                                                                       |                                                                                                                                                                    |
| ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| <img src="docs/pocketpuck.jpg" alt="An image of the PocketPuck device" width="400" /> | A body for Puck, [Amp](https://ampcode.com)'s companion. Synchronises your current thread state and lets you know if you need to take action. It's also very cute. |

Full video demo:

<video controls src="https://github.com/user-attachments/assets/eebd0190-7875-4f72-867d-91fb097cefb3"></video>

## Usage

Once connected, PocketPuck will automatically synchronise with your current Amp thread state. It will let you know if you have any threads waiting for you or tell you that there are new messages waiting for you (= a thread probably finished).

You press the dial once to open a thread overview that allows you to scroll through your threads. Pressing the dial again will then show a little bit more information on that thread. Long press the dial to get back to the main screen.

To open the Settings, long press the dial from the main screen. You can change the display fae, change whether Puck should blink or not, and reset all settings. Settings are persisted. There are a total of four display faces to choose from: from a very minimal one to reduce distractions to what you can see in the video (and even more unhinged).

## Hardware

- Arduino Nano ESP32 (any ESP32 will work, but you might have to change pinout assignments)
- [Waveshare 2inch LCD Module](https://www.waveshare.com/wiki/2inch_LCD_Module)
  (240 x 320, ST7789V)
- Rotary encoder with push button (KY-040 or similar)

### 3D Models

You can find the 3d models in the `models` directory.

| Model Name        | Description                                      |
| ----------------- | ------------------------------------------------ |
| `puck_manual.stl` | The main body of the PocketPuck.                 |
| `puck_hat.stl`    | A cute cowboy hat.                               |
| `puck_obelisk`    | A small obelisk to put on top of the PocketPuck. |
| `puck_orb.stl`    | An orb as a hat.                                 |
| `puck_knob.stl`   | An interestingly shaped knob as a hat.           |

You can hot glue the display to the inside of the body or use tape to secure it. The rotary dial goes through the hole and is fastened with a nut.

### Wiring

Disconnect USB power while wiring. The table below refers to the Arduino Nano ESP32 pin labels, not the normal ESP32 GPIO pinout.

| Waveshare LCD | Nano ESP32     | Purpose                |
| ------------- | -------------- | ---------------------- |
| `VCC`         | `3V3`          | Power                  |
| `GND`         | `GND`          | Ground                 |
| `DIN`         | `D11` / `COPI` | SPI data to display    |
| `CLK`         | `D12` / `CIPO` | Remapped SPI clock     |
| `CS`          | `D10`          | Chip select            |
| `DC`          | `D7`           | Data/command select    |
| `RST`         | `D8`           | Display reset          |
| `BL`          | `D9`           | Backlight, active high |

| Rotary encoder | Nano ESP32 | Purpose                                                                |
| -------------- | ---------- | ---------------------------------------------------------------------- |
| `CLK`          | `D2`       | Rotation clock                                                         |
| `DT`           | `D3`       | Rotation direction                                                     |
| `SW`           | `D4`       | Push switch                                                            |
| `+`            | `D5`       | Module pull-up voltage (you should connect this to 3V3 but I was lazy) |
| `GND`          | `GND`      | Common ground                                                          |

### Build and upload

[PlatformIO](https://platformio.org/) is the only required development tool.

```sh
pio run
pio run --target upload
pio device monitor
```

PlatformIO should automatically find the Nano ESP32's USB port. If an upload
cannot find the board, double-press RESET to enter the Arduino bootloader (the
green LED pulses), then run the upload command again.

The display and rotary pin assignments live in
[`include/display_config.h`](include/display_config.h).
The firmware uses the Adafruit GFX and ST7789 libraries, fetched automatically
by PlatformIO.

## Live Amp stats

Amp's API does not support all the thread information that I wanted to display.
PocketPuck therefor uses Rivet endpoint directly to get more detailed information,
meaning this can break at any time since there is no published API contract for this.
The bridge has a fallback to the `amp top` command, which is more stable but less detailed.

The bridge should be deployed on in the same network as PocketPuck. The server
serves traffic over HTTP on port 8765 by default. It is written in [Bun](https://bun.sh/).

I personally run this on a Raspberry Pi, where I also run Amp, meaning that I can use my
existing credentials. You can provide a custom API key using the `AMP_API_KEY` environment variable.

```sh
$HOME/.bun/bin/bun install --frozen-lockfile --production --ignore-scripts
```

Start the bridge service and query it with:

```sh
$HOME/.bun/bin/bun scripts/pocketpuck_bridge.mjs \
  --amp-command "$HOME/.amp/bin/amp"
curl http://localhost:8765/stats
```

By default, the bridge will try to find credentials in

- `$HOME/.config/amp/secrets.json`
- `$XDG_CONFIG_HOME/amp/secrets.json`
- `$HOME/.local/share/amp/secrets.json`

To run the bridge as a systemd user service, use a unit like this (adjust the
repository path if the checkout is not `%h/pocketpuck`):

```ini
[Unit]
Description=PocketPuck Amp bridge
After=network-online.target

[Service]
WorkingDirectory=%h/pocketpuck
ExecStart=%h/.bun/bin/bun scripts/pocketpuck_bridge.mjs --amp-command %h/.amp/bin/amp
# Optional custom-deployment overrides; normal Amp file credentials are discovered.
EnvironmentFile=-%h/.config/pocketpuck/environment
Restart=always

[Install]
WantedBy=default.target
```

## Connecting Your PocketPuck to the Bridge

Copy the example network configuration file to a local file that you can edit:

```sh
cp include/network_config.example.h include/network_config.h
```

Edit the `POCKETPUCK_WIFI_SSID`, `POCKETPUCK_WIFI_PASSWORD`, and `POCKETPUCK_STATS_URL` (for example `http://192.168.178.123:8765/stats`) values accordingly. Note that most ESP32 only support 2.4GHz WiFi networks, so make sure your router is configured correctly.

## FAQ (nobody has asked me anything yet)

> Why?

I sometimes start threads and then go on to do other things (definitely not procrastinating). I wanted a way to know if I have threads waiting for me. So instead of enabling notifications on my phone, I decided to build something completely unnecessary.

> Why is he so thick?

I overestimated how much space I needed inside and I have not modelled anything in Blender for a long time. This was supposed to just take a few days and I wanted to stick to that timeline.
The display being inset so deeply is also a result of me not having enough patience to do things properly in Blender. I've been using OnShape for most of my things recently and am just out of practice.

> Why do his hands look so weird?

It was my first time ever trying to even use the Blender sculpting tools and I regret everything.
