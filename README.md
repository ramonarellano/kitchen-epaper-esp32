# Kitchen E-Paper ESP32

This is the PlatformIO/Arduino firmware for the NodeMCU-32S (ESP32) board that bridges the cloud renderer and the RP2040 display controller.

## Overview

The ESP32 is the **timing master** for the system. It is responsible for:

- controlling Pico power via GPIO 25 + MOSFET (power cycling between refresh cycles)
- waiting for `SENDIMG` from the Pico after powering it on
- turning Wi-Fi on only when needed
- downloading a raw image from the cloud renderer over HTTPS
- streaming the image to the Pico as a framed UART payload
- waiting for `PICODONE` from the Pico to confirm display completion
- powering off the Pico and waiting 5 minutes before the next cycle
- storing persistent debug logs in SPIFFS

Current hardware and transport details:

- Board: NodeMCU-32S
- Framework: Arduino via PlatformIO
- `Serial1` pins: GPIO16 RX, GPIO17 TX
- Pico power pin: GPIO25 (drives N-channel MOSFET gate)
- UART baud: `115200`
- Image endpoint: `https://europe-north1-kitche-epaper-renderer.cloudfunctions.net/epaper?format=raw`

## Current UART Protocol

The ESP32 is the power-cycling master. Each cycle:

1. ESP32 powers on Pico (GPIO 25 HIGH), waits 3s for boot.
2. Pico boots, flushes `PLOG:` lines, sends `SENDIMG\n`.
3. ESP32 connects Wi-Fi, replies with `ACK\n` twice.
4. ESP32 streams: SOF `0xAA 0x55 0xAA 0x55` + 4-byte big-endian size + raw image bytes.
5. Pico receives image, inits panel, displays, sleeps panel.
6. Pico flushes final `PLOG:` lines, sends `PICODONE\n`.
7. ESP32 powers off Pico (GPIO 25 LOW).
8. ESP32 waits 5 minutes, then repeats.

Safety: 120s timeout after power-on; if `PICODONE` isn't received, ESP32 forces power off.

The ESP32 stores Pico `PLOG:` lines with a `[PICO]` prefix in the persistent log.

## Main Features

- Deferred Wi-Fi activation — Wi-Fi is kept off at boot and only activated during image transfer.
- Power-cycling master — ESP32 controls Pico power via GPIO 25 + MOSFET, guaranteeing a cold start every cycle.
- Explicit `WiFiClientSecure` with `setInsecure()` for the cloud-function HTTPS endpoint.
- Persistent SPIFFS logging that survives reboot and power loss.
- `PICODONE` handshake with 120s safety timeout.
- Wi-Fi auto-reboot after 3 consecutive failures.
- USB serial `GETLOG`/`CLEARLOG` commands available during idle wait.

## Serial Interfaces

- USB `Serial` at `115200`
  - debug output
  - `GETLOG`
  - `CLEARLOG`

- `Serial1` at `115200`
  - `SENDIMG` from the Pico
  - framed ACK/SOF/length/payload response back to the Pico
  - Pico `PLOG:` diagnostic lines

## Persistent Debug Logging

The ESP32 stores debug events in SPIFFS.

- Log file on device: `/debug_log.txt`
- Boot counter file on device: `/boot_count.txt`
- Maximum log size: 64KB, trimmed from the oldest entries when full

Important behavior:

- Logs are not cleared automatically on boot.
- Plugging the ESP32 into a computer for log retrieval usually adds a fresh boot entry at the very end of the fetched log.
- That trailing boot should not, by itself, be treated as evidence of an in-run reset.

## Fetching Logs

Always fetch ESP32 logs with `fetch_and_clear_logs.py`.

Fetch and clear:

```sh
python3 fetch_and_clear_logs.py
```

Fetch without clearing only if you intentionally need a snapshot:

```sh
python3 fetch_and_clear_logs.py --no-clear --timeout 60
```

Local log files are saved to `logs/esp32-debug-YYYYMMDD-HHMMSS.log`.

Do not use ad-hoc serial reads when collecting debugging evidence.

## Power Management And Reliability

The ESP32 is the power-cycling master for the whole system.

- **Pico power control** — GPIO 25 drives an N-channel MOSFET gate. HIGH = Pico on, LOW = Pico off. Starts LOW on boot (Pico off).
- **Cold start guarantee** — By cutting Pico power between cycles, all panel register decay / analog rail drift issues (Bug #15) are eliminated.
- **Deferred Wi-Fi** — Wi-Fi is only activated during image transfer and disconnected immediately after.
- **Wi-Fi auto-reboot** — after 3 consecutive Wi-Fi failures the ESP32 reboots itself with `ESP.restart()`.
- **Safety timeout** — if `PICODONE` is not received within 120s of power-on, the ESP32 forces Pico power off.
- **RTC-level reset logging** — boot logs include both the reset reason and the RTC reset reason.

## Cross-Board Logging (PLOG)

The Pico sends PLOG lines over `Serial1`, and the ESP32 persists them with a `[PICO]` prefix.

Current Pico-side events commonly seen in the stored log include:

- `BOOT vbus=X fw=POWER_CYCLE_v1`
- `SENDIMG_START attempt=X`
- `SENDIMG_RESULT rc=X recv=Y attempt=Z`
- `FULL_REINIT`
- `REINIT_DONE`
- `POWER_ON_PRE`
- `DISPLAY_DONE`
- `EPD_PHASES`
- `EPD_BUSY04`
- `EPD_BUSY12`
- `REFRESH_VERDICT`
- `EPD_SLEEP`
- `RECV_TIMEOUT`
- `RECV_FAIL`

ESP32-side events:

- `CYCLE_START #N`
- `PICO_POWER_ON`
- `SENDIMG command received`
- `Image streamed successfully`
- `PICODONE received`
- `PICO_POWER_OFF`
- `CYCLE_DONE`
- `CYCLE_FAIL` (with reason)
- `IDLE_WAIT Xs until next cycle`

Example log shape:

```text
[2026-04-23 19:46:31] [PICO] | SENDIMG_RESULT rc=0 recv=192000
[2026-04-23 19:46:32] [PICO] | POWER_ON_PRE rc=0 busy=1->0 attempt=1
[2026-04-23 19:46:32] [PICO] | DISPLAY_DONE ms=31207 forced=0 rc=0
[2026-04-23 19:46:32] [PICO] | EPD_PHASES pwr_on=0 refresh=30610 pwr_off=150
[2026-04-23 19:46:32] [PICO] | EPD_BUSY04 1->0
[2026-04-23 19:46:32] SENDIMG command received | source=Serial1
```

## Build And Flash

Firmware:

```sh
pio run --target upload
```

SPIFFS filesystem:

```sh
pio run --target uploadfs
```

If the board does not already have the `.env` file in SPIFFS, upload the filesystem before relying on Wi-Fi.

## Wi-Fi Credentials Setup

Wi-Fi credentials are loaded from `/data/.env` in SPIFFS rather than being hardcoded in firmware.

Create `data/.env`:

```ini
WIFI_SSID=your_wifi_ssid
WIFI_PASS=your_wifi_password
```

Then upload SPIFFS:

```sh
pio run --target buildfs
pio run --target uploadfs
```

`data/.env` is gitignored and should not be committed.
