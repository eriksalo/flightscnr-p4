#!/usr/bin/env python3
"""Capture serial output with host timestamps, without resetting the board.

Opening the port asserts DTR/RTS and reboots the chip, which hides whatever you
were trying to observe, so both are cleared before and immediately after open.

    python tools/serial_capture.py 60 > run.log
    python tools/serial_capture.py 60 COM5

Timestamps are seconds since capture start, which is what makes frame pacing
measurable: consecutive "[sweep] reveal ang=A->B" lines give the real sweep rate
(360 deg / 6 s target), and gaps between lines expose loop stalls.

Needs pyserial; the PlatformIO venv already has it:
    ~/.platformio/penv/Scripts/python.exe tools/serial_capture.py 60
"""
from __future__ import annotations

import sys
import time

import serial  # pyserial


def main() -> int:
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    port = sys.argv[2] if len(sys.argv) > 2 else "COM5"

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.setDTR(False)
    ser.setRTS(False)

    start = time.time()
    end = start + duration
    pending = b""
    try:
        while time.time() < end:
            chunk = ser.read(4096)
            if not chunk:
                continue
            stamp = time.time() - start
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip()
                print(f"{stamp:8.3f} {text}", flush=True)
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
