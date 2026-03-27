#!/bin/sh
# Build and flash both SPIFFS and firmware to ESP32. Open the Serial Monitor from the PlatformIO sidebar or VS Code command palette after flashing.

set -eu

if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
  # PlatformIO filesystem tools for ESP32 are often x86_64-only on macOS.
  # Without Rosetta, uploadfs fails later with "Bad CPU type in executable".
  if ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
    echo "[ERROR] Rosetta is required to build/upload SPIFFS on Apple Silicon."
    echo "[ERROR] Install it once with: softwareupdate --install-rosetta --agree-to-license"
    exit 1
  fi
fi

if command -v platformio >/dev/null 2>&1; then
  PIO_CMD="platformio"
elif command -v pio >/dev/null 2>&1; then
  PIO_CMD="pio"
elif [ -x "$HOME/.platformio/penv/bin/platformio" ]; then
  PIO_CMD="$HOME/.platformio/penv/bin/platformio"
else
  echo "[ERROR] PlatformIO CLI not found in PATH (expected 'platformio' or 'pio')."
  exit 1
fi

# Allow overriding from env, otherwise auto-detect common macOS USB serial names.
SERIAL_PORT="${SERIAL_PORT:-}"

if [ -z "$SERIAL_PORT" ]; then
  for candidate in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART /dev/cu.wchusbserial*; do
    if [ -e "$candidate" ]; then
      SERIAL_PORT="$candidate"
      break
    fi
  done
fi

if [ -z "$SERIAL_PORT" ]; then
  echo "[ERROR] No ESP32 serial port found. Set SERIAL_PORT and retry."
  exit 1
fi

# Stop any running PlatformIO serial monitor for the specific port
MON_PID=$(pgrep -f "(platformio|pio) device monitor.*$SERIAL_PORT" || true)
if [ -n "$MON_PID" ]; then
  echo "[INFO] Stopping PlatformIO serial monitor on $SERIAL_PORT (PID $MON_PID)..."
  kill "$MON_PID"
  sleep 1
else
  echo "[INFO] No PlatformIO serial monitor running on $SERIAL_PORT (pgrep)."
fi

# Extra: Kill any process using the serial port (e.g., lsof fallback)
LPORT_PID=$(lsof -t "$SERIAL_PORT" || true)
if [ -n "$LPORT_PID" ]; then
  echo "[INFO] Killing process using $SERIAL_PORT (PID $LPORT_PID)..."
  kill "$LPORT_PID"
  sleep 1
else
  echo "[INFO] No process using $SERIAL_PORT (lsof)."
fi

"$PIO_CMD" run --target uploadfs --upload-port "$SERIAL_PORT"
"$PIO_CMD" run --target upload --upload-port "$SERIAL_PORT"

echo "[INFO] Build and upload complete. Open the Serial Monitor from the PlatformIO sidebar or with Cmd+Shift+P → 'PlatformIO: Monitor'."
