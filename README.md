# Kitchen E-Paper ESP32

This is a PlatformIO project for the NodeMCU-32S (ESP32) board. It implements a UART handshake and image transfer protocol for use with a Raspberry Pi Pico (RP2040) e-paper display controller.

## Features Implemented

- **UART Handshake:**
  - Uses Serial1 (GPIO16=RX, GPIO17=TX) for handshake with the RP2040.
  - Waits for `HELLO ESP32` from the RP2040, replies with `HELLO RP2040`.
  - Retries handshake indefinitely until successful.
  - Debug output to USB serial (Serial) shows all received bytes and handshake status.

- **Image download from cloudfunctions.net:**
  - Uses the cloud application kitchen-epaper-renderer running in Azure
  - Retrieves a RAW image through a URL, but the URL can be tested in a browser by asking for a PNG:
  <https://europe-north1-kitche-epaper-renderer.cloudfunctions.net/epaper?format=png>

- **Image Transfer:**
  - Listens for the command `SENDIMG` on USB serial (Serial).
  - Responds by sending a hardcoded 800x480 1bpp 2x2 checkerboard pattern image buffer.
  - LED blinks rapidly during image transfer.

- **LED Status:**
  - Onboard LED blinks slowly while idle (waiting for commands).
  - Blinks rapidly during image transfer.

- **PlatformIO/Arduino Best Practices:**
  - Uses Arduino framework and PlatformIO conventions.
  - All commands are run directly for reproducibility.

## Getting Started

1. Install [PlatformIO](https://platformio.org/) in VS Code.
2. Connect your NodeMCU-32S (ESP32) board.
3. Open this project folder in VS Code.
4. Build and upload the firmware using the PlatformIO toolbar or the command palette.

## File Structure

- `platformio.ini`: PlatformIO configuration for the ESP32 board.
- `src/main.cpp`: Main application code (UART handshake, image transfer, LED logic).

## Serial Monitor

- USB Serial (Serial): 115200 baud (for debug and `SENDIMG` command)
- Serial1 (GPIO16/17): 115200 baud (for handshake with RP2040)

## Persistent Debug Logging (Standalone Overnight)

The firmware stores debug events in SPIFFS so logs survive reboot/power cycle.

- Log file on device: `/debug_log.txt`
- Boot counter file on device: `/boot_count.txt`
- Maximum log size: 64KB (oldest entries are trimmed when full)
- USB commands:
  - `GETLOG` prints the stored log
  - `CLEARLOG` deletes the stored log

Important behavior:

- Logs are appended on boot; they are not cleared automatically when you plug the ESP32 into a computer.
- SPIFFS mount is configured without auto-format, so a mount failure will not erase logs.

## Power Management and Reliability

The ESP32 shares a power rail with the RP2040 and the 7.3" e-paper panel. The panel refresh draws significant current, which can cause full power-on resets (voltage collapse) on the ESP32. The following mitigations are in place:

- **Deferred WiFi** — WiFi is kept off (`WiFi.mode(WIFI_OFF)`) at boot and only activated when a `SENDIMG` command is received. This eliminates idle radio power draw (~150mA continuous) during the ~58 minute wait between image requests, preventing TX current spikes from contributing to voltage dips on the shared power rail.
- **WiFi disconnect on failure** — if image streaming fails, WiFi is explicitly disconnected and the radio is turned off (`WiFi.disconnect(true)` + `WiFi.mode(WIFI_OFF)`) to avoid leaving the radio active during idle wait.
- **Guarded WiFi maintenance** — background WiFi reconnection (`maintain_wifi_connection()`) is gated by a `wifiActivated` flag so it only runs after an intentional connect, preventing it from re-enabling the radio during the idle wait period.
- **No deep sleep** — the ESP32 stays awake continuously, listening for SENDIMG on Serial1. After streaming an image, WiFi is disconnected but UART remains active. This eliminates Bug #14 (POWERON resets during deep sleep) and all timing synchronization issues between the ESP32 and Pico. Power draw is higher (~30-40mA idle vs ~10µA in deep sleep) but acceptable for a wall-powered device.
- **RTC-level reset reason logging** — boot logs include both the IDF reset reason (`esp_reset_reason()`) and the hardware RTC reset reason (`rtc_get_reset_reason()`). This distinguishes true power-on resets (`rtc_reason=1`) from brownout resets (`rtc_reason=15`) and watchdog resets, aiding diagnosis of power rail issues.
- **Brownout detector** — left enabled (default). Disabling it masks the underlying power issue and risks silent corruption. The includes for toggling it are kept in `main.cpp` for diagnostic use. To disable temporarily, add `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);` at the top of `setup()`.
- **WiFi auto-reboot** — after 3 consecutive WiFi connection failures, the ESP32 reboots itself (`ESP.restart()`) to recover from stuck radio states.
- **Stale command drain** — queued SENDIMG commands are drained from the UART buffer before processing, preventing a backlog from blocking the main loop.
- **Explicit HTTPS client** — uses `WiFiClientSecure` (with `setInsecure()`) for proper TLS handling of the cloud function endpoint.
- **NTP time sync** — after WiFi connects, time is synced via `pool.ntp.org` using the Europe/Oslo timezone (`CET-1CEST`) so all subsequent log entries use local wall-clock timestamps.
- **Idle heartbeat** — while waiting for SENDIMG, the ESP32 logs a heartbeat every 5 minutes (`IDLE_HEARTBEAT uptime=Xs heap=Y serial1=Z`) to confirm it is alive and listening.

## Cross-Board Logging (PLOG)

The Pico sends diagnostic log lines prefixed with `PLOG:` over UART. The ESP32 recognizes these and stores them in the persistent SPIFFS log with a `[PICO]` prefix. This gives full visibility into the Pico's behavior (display updates, duplicate skips, timeouts) without needing a separate USB connection to the Pico.

Example log output:
```
[2026-04-16 21:30:00] [PICO] | DISPLAY chk=12345 bytes=192000 first4=11111111
[2026-04-16 21:30:00] [PICO] | DISPLAY_DONE ms=31387 forced=0
[2026-04-16 21:30:00] [PICO] | EPD_PHASES pwr_on=200 refresh=28000 pwr_off=300
[2026-04-16 21:30:00] [PICO] | REFRESH_VERDICT real=1 refresh_ms=28000
[2026-04-16 21:30:00] [PICO] | NO_DEEP_SLEEP last_sum=0
[2026-04-16 21:30:00] SENDIMG command received | source=Serial1
[2026-04-16 21:30:02] NTP time: 2026-04-16 21:30:02 Oslo
```

Pico log events: `BOOT`, `SENDIMG_START`, `SENDIMG_RESULT`, `EPD_INIT`, `INIT_DONE`, `DISPLAY`, `DISPLAY_DONE`, `EPD_PHASES`, `REFRESH_VERDICT`, `NO_DEEP_SLEEP`, `WAIT_START`, `WAIT_TICK`, `WAIT_DONE`, `SKIP`, `RECV_TIMEOUT`, `RECV_FAIL`.

ESP32 log events: `IDLE_HEARTBEAT`.

## Export Stored Logs To Host File

Use the scripts in this repo to dump stored logs into the local `logs/` folder with a timestamped filename.

### Quick: download and clear in one step

```sh
python3 fetch_and_clear_logs.py
```

This downloads the log, saves it to `logs/esp32-debug-YYYYMMDD-HHMMSS.log`, and clears the ESP32's persistent log. Use `--no-clear` to download without clearing.

### Download only (legacy script)

```sh
python3 export_esp32_logs.py
```

## WiFi Credentials Setup

WiFi credentials are **not hardcoded** in the firmware. Instead, they are loaded at runtime from a `.env` file stored in SPIFFS. This allows you to keep your credentials private and out of version control.

### How to set up WiFi credentials

1. Create a file named `.env` in the `/data` directory (see `.env.example` for the format):

   ```ini
   WIFI_SSID=your_wifi_ssid
   WIFI_PASS=your_wifi_password
   ```

2. Upload the `/data/.env` file to the ESP32's SPIFFS filesystem:

   - Build the SPIFFS image:

     ```sh
     pio run --target buildfs
     ```

   - Upload the SPIFFS image to the device:

     ```sh
     platformio run --target uploadfs
     ```

3. The firmware will automatically read the WiFi credentials from `/data/.env` on boot.

> **Note:** `.env` and `/data/.env` are included in `.gitignore` and will not be committed to version control.

---

For more details, see the PlatformIO and Arduino documentation.
