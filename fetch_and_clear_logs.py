#!/usr/bin/env python3
"""Download ESP32 debug logs to ./logs and clear them on the device.

Usage:
  python3 fetch_and_clear_logs.py
  python3 fetch_and_clear_logs.py --port /dev/cu.usbserial-0001
  python3 fetch_and_clear_logs.py --no-clear   # download only, don't clear
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
CLEAR_ACK = "Persistent log cleared"


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


def clear_log(ser: serial.Serial, timeout_s: float = 5.0) -> bool:
    ser.read(ser.in_waiting)  # drain
    ser.write(b"CLEARLOG\n")
    ser.flush()

    deadline = time.time() + timeout_s
    buffer = ""
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            buffer += ser.read(n).decode("utf-8", errors="replace")
            if CLEAR_ACK in buffer:
                return True
        else:
            time.sleep(0.05)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download ESP32 debug logs and optionally clear them"
    )
    parser.add_argument("--port", help="Serial port (auto-detect if omitted)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument(
        "--timeout", type=float, default=20.0, help="Seconds to wait for log dump"
    )
    parser.add_argument(
        "--no-clear",
        action="store_true",
        help="Download logs without clearing them on the ESP32",
    )
    args = parser.parse_args()

    port = args.port or detect_port()
    if not port:
        print("No serial port detected. Use --port /dev/cu.usbserial-XXXX")
        return 1

    logs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    os.makedirs(logs_dir, exist_ok=True)

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_path = os.path.join(logs_dir, f"esp32-debug-{timestamp}.log")

    print(f"Opening serial port: {port} @ {args.baud}")
    with serial.Serial(port=port, baudrate=args.baud, timeout=0.2) as ser:
        ser.dtr = False
        time.sleep(0.05)
        ser.dtr = True
        time.sleep(0.25)

        print("Fetching logs...")
        log_text = fetch_log(ser, timeout_s=args.timeout)

        with open(out_path, "w", encoding="utf-8") as f:
            f.write(log_text)
        print(f"Saved log to: {out_path}")

        if not args.no_clear:
            print("Clearing ESP32 logs...")
            if clear_log(ser):
                print("ESP32 logs cleared.")
            else:
                print("WARNING: Did not receive clear confirmation from ESP32.")
                return 1
        else:
            print("Skipping log clear (--no-clear).")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
