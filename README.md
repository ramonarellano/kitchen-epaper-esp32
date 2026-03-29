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

The ESP32 shares a power rail with the RP2040 and the 7.3" e-paper panel. The panel refresh draws significant current, which can cause brownout resets on the ESP32. The following mitigations are in place:

- **Brownout detector disabled** — prevents infinite reboot loops when the display refresh causes a voltage dip. Added via `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` in `setup()`.
- **Deep sleep after image transfer** — after successfully streaming the image to the RP2040, the ESP32 enters deep sleep for 2 minutes. This drops power draw to ~10µA during the panel refresh window, giving the display the full power budget.
- **WiFi auto-reboot** — after 3 consecutive WiFi connection failures, the ESP32 reboots itself (`ESP.restart()`) to recover from stuck radio states.
- **Stale command drain** — queued SENDIMG commands are drained from the UART buffer before processing, preventing a backlog from blocking the main loop.
- **Explicit HTTPS client** — uses `WiFiClientSecure` (with `setInsecure()`) for proper TLS handling of the cloud function endpoint.

## Export Stored Logs To Host File

Use the script in this repo to dump stored logs into the local `logs/` folder with a timestamped filename.

1. Install dependency:

  ```sh
  python3 -m pip install pyserial
  ```

1. Run export script:

  ```sh
  python3 export_esp32_logs.py
  ```

1. Optional: choose serial port manually:

  ```sh
  python3 export_esp32_logs.py --port /dev/cu.usbserial-0001
  ```

Output file example:

- `logs/esp32-debug-20260329-093000.log`

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
