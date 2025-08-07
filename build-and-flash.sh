#!/bin/sh
# Build and flash both SPIFFS and firmware to ESP32. Open the Serial Monitor from the PlatformIO sidebar or VS Code command palette after flashing.

SERIAL_PORT="/dev/tty.SLAB_USBtoUART"

# Stop any running PlatformIO serial monitor for the specific port
MON_PID=$(pgrep -f "platformio device monitor.*$SERIAL_PORT")
if [ -n "$MON_PID" ]; then
  echo "[INFO] Stopping PlatformIO serial monitor on $SERIAL_PORT (PID $MON_PID)..."
  kill "$MON_PID"
  sleep 1
else
  echo "[INFO] No PlatformIO serial monitor running on $SERIAL_PORT (pgrep)."
fi

# Extra: Kill any process using the serial port (e.g., lsof fallback)
LPORT_PID=$(lsof -t "$SERIAL_PORT")
if [ -n "$LPORT_PID" ]; then
  echo "[INFO] Killing process using $SERIAL_PORT (PID $LPORT_PID)..."
  kill "$LPORT_PID"
  sleep 1
else
  echo "[INFO] No process using $SERIAL_PORT (lsof)."
fi

platformio run --target uploadfs && platformio run --target upload

echo "[INFO] Build and upload complete. Open the Serial Monitor from the PlatformIO sidebar or with Cmd+Shift+P → 'PlatformIO: Monitor'."
