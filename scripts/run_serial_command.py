#!/usr/bin/env python3
"""Run one command through the STM32 Buildroot serial console, with pacing."""

import argparse
import os
import re
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


def paced_write(fd, value, delay):
    for byte in value.encode():
        while True:
            try:
                os.write(fd, bytes((byte,)))
                break
            except BlockingIOError:
                # ST-LINK VCP can temporarily deassert host-side writable
                # state while the small target UART FIFO drains.
                select.select([], [fd], [], 1.0)
        time.sleep(delay)


def read_until(fd, stream, captured, patterns, deadline):
    compiled = [re.compile(pattern) for pattern in patterns]
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 1.0)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            # A VMIN=0 tty may report readable and then return zero bytes.
            # Without a backoff this loop consumes a host CPU and can delay
            # effective USB-VCP delivery during large benchmark snapshots.
            time.sleep(0.01)
            continue
        stream.write(chunk)
        stream.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        captured.extend(chunk)
        window = bytes(captured[-16384:])
        for index, pattern in enumerate(compiled):
            if pattern.search(window):
                return index
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--command", required=True)
    parser.add_argument("--username", default="root")
    parser.add_argument("--password-env", default="SERIAL_CONSOLE_PASSWORD")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--character-delay-ms", type=float, default=100.0)
    parser.add_argument(
        "--require-login", action="store_true",
        help="ignore stale shell prompts and wait for a fresh login prompt",
    )
    args = parser.parse_args()
    if not args.device.startswith("/dev/") or not os.path.exists(args.device):
        parser.error("--device must name an existing exact /dev path")
    password = os.environ.get(args.password_env)
    if password is None:
        parser.error(f"password environment variable {args.password_env} is unset")

    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)
    termios.tcflush(fd, termios.TCIFLUSH)
    delay = args.character_delay_ms / 1000.0
    deadline = time.monotonic() + args.timeout
    captured = bytearray()
    marker = f"__SERIAL_COMMAND_RETURN__:{os.getpid()}:"
    with args.output.open("wb") as stream:
        paced_write(fd, "\r", delay)
        patterns = (rb"(?:^|[\r\n])[A-Za-z0-9._-]+ login: ",)
        if not args.require_login:
            patterns += (rb"(?:^|[\r\n])~ # ",)
        state = read_until(fd, stream, captured, patterns, deadline)
        if state is None:
            raise SystemExit("neither login nor shell prompt was observed")
        if state == 0:
            paced_write(fd, args.username + "\r", delay)
            if read_until(fd, stream, captured, (rb"Password: ",), deadline) is None:
                raise SystemExit("password prompt was not observed")
            paced_write(fd, password + "\r", delay)
            if read_until(
                fd, stream, captured, (rb"(?:^|[\r\n])~ # ",), deadline
            ) is None:
                raise SystemExit("shell prompt was not observed after login")

        shell_command = (
            f"{args.command}; serial_status=$?; "
            f"printf '{marker}%s\\n' \"$serial_status\"\r"
        )
        paced_write(fd, shell_command, delay)
        result_pattern = re.escape(marker.encode()) + rb"([0-9]+)"
        if read_until(fd, stream, captured, (result_pattern,), deadline) is None:
            raise SystemExit("command return marker was not observed before timeout")

    os.close(fd)
    matches = re.findall(result_pattern, bytes(captured))
    if not matches:
        raise SystemExit("command return marker was lost")
    return int(matches[-1])


if __name__ == "__main__":
    raise SystemExit(main())
