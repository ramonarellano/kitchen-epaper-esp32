# Kitchen E-Paper ESP32

This is a PlatformIO project for the NodeMCU-32S (ESP32) board. It implements a UART handshake and image transfer protocol for use with a Raspberry Pi Pico (RP2040) e-paper display controller.

## Features Implemented

- **UART Handshake:**
  - Uses Serial1 (GPIO16=RX, GPIO17=TX) for handshake with the RP2040.
  - Waits for `HELLO ESP32` from the RP2040, replies with `HELLO RP2040`.
  - Retries handshake indefinitely until successful.
  - Debug output to USB serial (Serial) shows all received bytes and handshake status.

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

---

For more details, see the PlatformIO and Arduino documentation.
