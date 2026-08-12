#!/usr/bin/env python3
"""Capture an explicitly selected 115200-8N1 serial port until login."""

import argparse
import os
import select
import sys
import termios
import time
from pathlib import Path


def configure(fd):
    attributes = termios.tcgetattr(fd)
    attributes[0] = 0
    attributes[1] = 0
    attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attributes[3] = 0
    attributes[4] = termios.B115200
    attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = 0
    attributes[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attributes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True,
                        help="exact serial path discovered under /dev")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if not args.device.startswith("/dev/"):
        parser.error("--device must be an exact /dev path")
    if not os.path.exists(args.device):
        parser.error(f"serial device is absent: {args.device}")

    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)
    deadline = time.monotonic() + args.timeout
    captured = bytearray()
    with args.output.open("wb") as stream:
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 1.0)
            if not readable:
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                continue
            stream.write(chunk)
            stream.flush()
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            captured.extend(chunk)
            if b"stm32f746-linux login:" in captured[-8192:]:
                break
    os.close(fd)
    if b"stm32f746-linux login:" not in captured:
        raise SystemExit("login prompt was not observed before timeout")


if __name__ == "__main__":
    main()
