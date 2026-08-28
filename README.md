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
- Five-pin rotary encoder module (`CLK`, `DT`, `SW`, `+`, `GND`)
- 13 jumper wires

### Wiring

Disconnect USB power while wiring. Use the **Nano pin labels** in this table,
not the ESP32-S3's raw GPIO numbers.

| Waveshare LCD | Nano ESP32 | Purpose |
| --- | --- | --- |
| `VCC` | `3V3` | Power |
| `GND` | `GND` | Ground |
| `DIN` | `D11` / `COPI` | SPI data to display |
| `CLK` | `D12` / `CIPO` | Remapped SPI clock |
| `CS` | `D10` | Chip select |
| `DC` | `D7` | Data/command select |
| `RST` | `D8` | Display reset |
| `BL` | `D9` | Backlight, active high |

| Rotary encoder | Nano ESP32 | Purpose |
| --- | --- | --- |
| `CLK` | `D2` | Rotation clock |
| `DT` | `D3` | Rotation direction |
| `SW` | `D4` | Push switch |
| `+` | `D5` | Module pull-up voltage |
| `GND` | `GND` | Common ground |

PocketPuck holds `D5` high at 3.3 V to supply the resistor-only encoder module's
small pull-up current, leaving the Nano's `3V3` pin available for the display.
Do not use this arrangement for a module with a motor, lamp, or other substantial
load, and never connect the encoder's `+` to 5 V. PocketPuck also enables the
Nano's internal pull-up resistors. If clockwise rotation lowers brightness on
your encoder, swap its `CLK` and `DT` wires.

On the animated face, turn the dial to adjust the LCD backlight and press it to
open the thread list. Hold the button for 700 ms to open the main menu. Select
**Select Face** to preview the Minimal, Knock, Beacon, and Panic designs with a
synchronized scripted status lifecycle; pressing confirms a choice that
persists across reboots. **Select Font** similarly previews and persists
Classic, IBM Plex Mono, Chakra Petch, and VT323 treatments for the prominent
display copy, plus Share Tech Mono, Audiowide, Rajdhani, and Quantico. Compact
labels retain the pixel font so dense screens remain readable. The **Settings**
submenu can disable Puck's blinking, open **Debug Face**, or reset all settings,
including the selected face and font, to their defaults. Debug Face holds the
currently selected face on a dial-selected fixture state—Idle, Working, Message,
Attention, or All Clear. Each dial change plays that state's normal transition,
then holds it instead of advancing on the normal timer; press the dial to return
to Settings. Outside the picker, the selected design uses live
Amp data. Working remains ambient, new
messages are noticeable, and actionable
states receive the strongest treatment while directing the user back to Amp.
In the thread list, turn to select a thread and press to see its full title,
project, state, unread status, and executor attachment. Turn on the detail page
to move between threads, short-press to return to the list, or hold the button
for 700 ms to return directly to the face. Manual browsing returns to the face
after 30 seconds without input.

The display is write-only, so PocketPuck remaps the hardware SPI clock to the
otherwise-unused `D12` pin. The Nano ESP32's yellow built-in LED shares `D13`
with the default SPI clock and would flash on every display update if `CLK`
were connected there.

The Nano ESP32 GPIOs use 3.3 V logic, so the display is powered from `3V3` to
keep its supply and logic voltages consistent. Do not connect a GPIO to 5 V.
The display does not return data, which is why its normal `CIPO` function is
not needed and `D12` can be reused as the clock output.

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

The display and rotary pin assignments live in
[`include/display_config.h`](include/display_config.h).
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

Detailed mode targets hosted production Amp. The bridge first uses a nonempty
`AMP_API_KEY`; otherwise it reads Amp's existing production key from
`$XDG_DATA_HOME/amp/secrets.json` or
`$HOME/.local/share/amp/secrets.json`. The store must be a same-user regular
file with no group/world access, and its exact
`apiKey@https://ampcode.com/` entry must contain a nonempty string. Native Amp
keychain storage cannot be read by the bridge, so those installations must use
the environment override. No duplicate environment file is needed for the
normal Pi setup.

The production bootstrap URL and public Rivet actor routing endpoint are fixed
in the bridge. The API key is never logged or written. `AMP_COMMAND`,
`POCKETPUCK_HOST`, and `POCKETPUCK_PORT` provide environment alternatives to
the command-line flags and defaults.

With the private integration configured, the response has this shape (the
`items` list is bounded to eight summaries):

```json
{"schemaVersion":2,"source":"user-actor","running":4,"idle":10,"states":{"idle":10,"compacting":0,"working":0,"streaming":1,"tool_use":0,"running_tools":2,"awaiting_approval":1,"error":0,"unknown":0},"unread":2,"shipping":1,"shipped":0,"executorConnected":6,"headline":{"working":3,"needsAttention":1,"idle":10},"items":[{"id":"T-123","title":"Build PocketPuck UI","project":"pocketpuck","state":"tool_use","executorConnected":true,"unread":false,"shipping":true,"shipped":false}],"updatedAt":"2026-08-26T21:02:57.697Z","reconnecting":false,"stale":false}
```

The bridge keeps one GLOBAL user-actor connection, loads a recent baseline,
applies `threadStatusUpdated` events, and refreshes the authoritative baseline
every 20 seconds so a silent event subscription cannot preserve old data
indefinitely. It exposes Amp's detailed
`idle`, `compacting`, `working`, `streaming`, `tool_use`, `running_tools`,
`awaiting_approval`, and `error` states. `hasUnreadMessages` is counted
separately as `unread` and shown as a blue dot in the thread overview.
Amp's Ship UI lifecycle is exposed independently as `shipping`. The user-actor
summary identifies candidates but does not carry this field, so the bridge
checks recent and active candidates with `amp threads export`; both the
`shipping` and `awaiting_commit` stages count as active. Puck shows that state
in the thread browser. A disappearing Ship state alone is not success because
pausing also clears it. The bridge emits a bounded `shipped` event, and Puck
announces **Shipped**, only after the thread ends normally and Amp applies its
system `shipped` label. This also covers Ship workflows that deploy or perform
another external action without creating a commit. It works with custom Ship
prompts and tools because it follows Amp's Ship UI lifecycle rather than
recognizing tool names.
`needsAttention` includes only approval-blocked and errored threads; unread does
not imply that a reply is required. Confirmation dialogs represented by Amp's
`indicator.kind: "action-required"` are normalized to `awaiting_approval` even
when their raw execution state remains `tool_use`. `running` preserves Amp's
compatibility mapping, including approval. The headline working, attention, and
idle counts are state-based, with active Ship lifecycle threads excluded from
working so the separate shipping count does not overlap it. Unread remains an
independent count that may overlap any of them. Executor attachment remains
orthogonal and is never interpreted as thread health.

For raw summaries, `project` is derived from the basename of `workspace.uri`,
matching Amp's display fallback. The optional `workspace.displayName` is kept
separately as `workspaceDisplayName` rather than silently substituting it.

If automatic discovery fails, private authentication fails, its response shape
changes, or retries are exhausted, the Bun bridge automatically spawns `amp top
--stream-jsonl`. Discovery failure messages contain only a non-secret invariant,
and the bridge retries discovery every five minutes. Fallback responses identify
`source: "amp-top"`, preserve
the verified `working × executorConnected` counts, and set `states` and `unread`
to `null` rather than pretending those details are zero. They can only label
items `WORKING` or `IDLE`. The experimental display-oriented `status` field is
never parsed. Neither source calls generic idle “waiting for input.” The bridge
retries private mode every five minutes and switches sources only after a full
snapshot; streams are never merged.

The bridge settles an initial empty stream event for two seconds, returns HTTP
503 until data is ready, and returns 503 whenever no event or successful
baseline refresh has arrived for 30 seconds. Aggregate transport reconnecting
state causes firmware to hide retained counts rather than displaying them as
current; marking a retained snapshot stale does not renew its freshness timer.

Configure the firmware with the Pi's LAN address:

```sh
cp include/network_config.example.h include/network_config.h
```

Edit `include/network_config.h`, then build and upload normally. This local
file is ignored by git. The display retries Wi-Fi every 15 seconds and polls
the bridge every 10 seconds. Startup keeps the animated logo visible until the
first bridge result, or a bounded 20-second attempt, plus another five seconds.
It then shows the face even when degraded, with clear states for setup, Wi-Fi,
bridge reconnects, and no threads. Pressing the rotary encoder opens an overview
with up to four thread titles, projects, states, and blue unread markers; long
fields are deterministically ellipsized and an overflow count represents
additional rows.

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
# Optional custom-deployment overrides; normal Amp file credentials are discovered.
EnvironmentFile=-%h/.config/pocketpuck/environment
Restart=always

[Install]
WantedBy=default.target
```
