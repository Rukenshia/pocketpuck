# PocketPuck

A tiny display companion for [Amp](https://ampcode.com), built with an Arduino
Nano ESP32 and a Waveshare 2inch LCD Module. Amp's wordmark rolls through its
dark splash animation while networking starts, then gives way to a living face
with prominent live thread counts.

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
state. PocketPuck therefore uses a small bridge which prefers Amp's private
GLOBAL user-actor summaries and falls back to the authenticated `amp top`
stream. The Amp API key remains on the bridge host rather than being copied to
the microcontroller.

On the Raspberry Pi, install dependencies with the standalone Bun runtime. Bun
1.4.0 on Linux ARM64 is the tested deployment runtime:

```sh
$HOME/.bun/bin/bun install --frozen-lockfile --production --ignore-scripts
```

Start the single bridge service and query it with:

```sh
$HOME/.bun/bin/bun scripts/pocketpuck_bridge.mjs \
  --amp-command "$HOME/.amp/bin/amp"
curl http://localhost:8765/stats
```

The Bun process owns the HTTP endpoint, detailed user-actor connection, cache,
reconnects, and the degraded `amp top --stream-jsonl` child process. No second
service is required.

To enable detailed states, provide `AMP_API_KEY` and the full
`RIVET_PUBLIC_ENDPOINT` in the bridge service environment. `AMP_URL` is optional
and defaults to `https://ampcode.com`. The hosted actor endpoint contains Amp's
public routing configuration; take the complete value from the current Amp
deployment rather than reconstructing or printing its compiled components. Keep
all values in a restricted environment file on the Pi, never in firmware or Git.
`AMP_COMMAND`, `POCKETPUCK_HOST`, and `POCKETPUCK_PORT` provide environment
alternatives to the command-line flags and defaults.

With the private integration configured, the response has this shape (the
`items` list is bounded to eight summaries):

```json
{"schemaVersion":2,"source":"user-actor","running":4,"idle":10,"states":{"idle":10,"compacting":0,"working":0,"streaming":1,"tool_use":0,"running_tools":2,"awaiting_approval":1,"error":0,"unknown":0},"unread":2,"executorConnected":6,"headline":{"working":3,"needsAttention":3,"idle":9},"items":[{"id":"T-123","title":"Build PocketPuck UI","project":"pocketpuck","state":"awaiting_approval","executorConnected":true,"unread":false}],"updatedAt":"2026-08-26T21:02:57.697Z","reconnecting":false,"stale":false}
```

The bridge keeps one GLOBAL user-actor connection, loads a recent
baseline, and applies `threadStatusUpdated` events. It exposes Amp's detailed
`idle`, `compacting`, `working`, `streaming`, `tool_use`, `running_tools`,
`awaiting_approval`, and `error` states. `hasUnreadMessages` is counted
separately as `attention.unread`; `NEW` means activity worth looking at, not that
a reply is required. `running` preserves Amp's compatibility mapping, including
approval; `unread` is an independent raw count and may overlap active states.
The disjoint `headline` buckets prioritize approval, error, unread, active work,
then idle for display. Executor attachment remains orthogonal and is never
interpreted as thread health.

For raw summaries, `project` is derived from the basename of `workspace.uri`,
matching Amp's display fallback. The optional `workspace.displayName` is kept
separately as `workspaceDisplayName` rather than silently substituting it.

If private credentials are absent, authentication fails, its response shape
changes, or retries are exhausted, the Bun bridge automatically spawns `amp top
--stream-jsonl`. Fallback responses identify `source: "amp-top"`, preserve
the verified `working × executorConnected` counts, and set `states` and `unread`
to `null` rather than pretending those details are zero. They can only label
items `WORKING` or `IDLE`. The experimental display-oriented `status` field is
never parsed. Neither source calls generic idle “waiting for input.” The bridge
retries private mode every five minutes and switches sources only after a full
snapshot; streams are never merged.

The bridge settles an initial empty stream event for two seconds, returns HTTP
503 until data is ready, and returns 503 whenever no event has arrived for 30
seconds. Aggregate transport reconnecting state causes firmware to hide retained
counts rather than displaying them as current.

Configure the firmware with the Pi's LAN address:

```sh
cp include/network_config.example.h include/network_config.h
```

Edit `include/network_config.h`, then build and upload normally. This local
file is ignored by git. The display retries Wi-Fi every 15 seconds and polls
the bridge every 10 seconds. Startup keeps the animated logo visible until the
first bridge result, or a bounded 20-second attempt, plus another five seconds.
It then shows the face even when degraded, with clear states for setup, Wi-Fi,
bridge reconnects, and no threads. Every 30 seconds a six-second overview shows
up to four thread titles with project and state; long fields are deterministically
ellipsized and an overflow count represents additional rows.

The bridge binds to `0.0.0.0` by default so the microcontroller can reach it on
the LAN. Do not expose port 8765 to the public Internet. Use `--host` to bind a
specific LAN address if preferred.

Run the bridge fixture tests with:

```sh
bun install --frozen-lockfile --ignore-scripts
bun test scripts/pocketpuck_bridge.test.mjs
```

These same commands work from any Mac checkout when standalone Bun is on
`PATH`; no repository path is compiled into the bridge.

To run the bridge as a systemd user service, use a unit like this (adjust the
repository path if the checkout is not `%h/pocketpuck`):

```ini
[Unit]
Description=PocketPuck Amp bridge
After=network-online.target

[Service]
WorkingDirectory=%h/pocketpuck
ExecStart=%h/.bun/bin/bun scripts/pocketpuck_bridge.mjs --amp-command %h/.amp/bin/amp
# Optional private integration; create this mode-0600 file outside Git.
EnvironmentFile=-%h/.config/pocketpuck/environment
Restart=always

[Install]
WantedBy=default.target
```
