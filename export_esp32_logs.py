#!/usr/bin/env python3
"""Export persisted ESP32 debug logs to ./logs with a timestamped filename.

Usage examples:
  python3 export_esp32_logs.py
  python3 export_esp32_logs.py --port /dev/cu.usbserial-0001
  python3 export_esp32_logs.py --timeout 25

Behavior:
- Opens USB serial at 115200.
- Waits briefly for boot output.
- Sends GETLOG command.
- Captures log section between markers.
- Saves to logs/esp32-debug-YYYYmmdd-HHMMSS.log.
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import os
import sys
import time

try:
    import serial
except ImportError:
    print("Missing dependency: pyserial")
    print("Install with: python3 -m pip install pyserial")
    sys.exit(2)


START_MARKER = "=== Debug Log Contents ==="
END_MARKER = "=== End of Log ==="


def detect_port() -> str | None:
    candidates = []
    for pattern in (
        "/dev/cu.usbserial-*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.wchusbserial*",
    ):
        candidates.extend(glob.glob(pattern))
    candidates = sorted(set(candidates))
    return candidates[0] if candidates else None


def read_until(ser: serial.Serial, deadline: float) -> str:
    chunks: list[str] = []
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            b = ser.read(n)
            chunks.append(b.decode("utf-8", errors="replace"))
        else:
            time.sleep(0.05)
    return "".join(chunks)


def fetch_log(ser: serial.Serial, timeout_s: float) -> str:
    # Drain initial boot chatter first.
    _ = read_until(ser, time.time() + 1.0)

    buffer = ""
    deadline = time.time() + timeout_s
    next_getlog_at = time.time()

    while time.time() < deadline:
        now = time.time()
        if now >= next_getlog_at:
            ser.write(b"GETLOG\n")
            ser.flush()
            next_getlog_at = now + 2.0

        n = ser.in_waiting
        if n > 0:
            chunk = ser.read(n).decode("utf-8", errors="replace")
            buffer += chunk
            if END_MARKER in buffer:
                break
        else:
            time.sleep(0.05)

    start = buffer.find(START_MARKER)
    end = buffer.find(END_MARKER)

    if start == -1 or end == -1 or end <= start:
        raise RuntimeError(
            "Did not receive full log markers. "
            "Make sure firmware supports GETLOG and serial settings are correct."
        )

    end += len(END_MARKER)
    return buffer[start:end] + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Export persisted ESP32 logs")
    parser.add_argument("--port", help="Serial port (auto-detect if omitted)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument(
        "--timeout", type=float, default=20.0, help="Seconds to wait for log dump"
    )
    args = parser.parse_args()

    port = args.port or detect_port()
    if not port:
        print("No serial port detected. Use --port /dev/cu.usbserial-XXXX")
        return 1

    logs_dir = os.path.join(os.path.dirname(__file__), "logs")
    os.makedirs(logs_dir, exist_ok=True)

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_path = os.path.join(logs_dir, f"esp32-debug-{timestamp}.log")

    print(f"Opening serial port: {port} @ {args.baud}")
    with serial.Serial(port=port, baudrate=args.baud, timeout=0.2) as ser:
        # Trigger a clean monitor-like session.
        ser.dtr = False
        time.sleep(0.05)
        ser.dtr = True
        time.sleep(0.25)

        log_text = fetch_log(ser, timeout_s=args.timeout)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(log_text)

    print(f"Saved log to: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
