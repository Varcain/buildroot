#!/usr/bin/env python3
"""Enter STM32 U-Boot over the exact serial console and run paced commands."""

import argparse
import os
import re
import select
import sys
import termios
import time
from pathlib import Path

from run_serial_command import configure, paced_write, read_until


LOGIN_PATTERN = rb"(?:^|[\r\n])[A-Za-z0-9._-]+ login: "
SHELL_PATTERN = rb"(?:^|[\r\n])~ # "
UBOOT_PATTERN = rb"(?:=>|U-Boot >) "


def wait_for(fd, stream, pattern, deadline):
    captured = bytearray()
    if read_until(fd, stream, captured, (pattern,), deadline) is None:
        raise SystemExit(f"serial pattern was not observed: {pattern!r}")


def stop_autoboot(fd, stream, deadline):
    """Send one exact SPACE after U-Boot publishes the autoboot prompt."""
    captured = bytearray()
    prompt = re.compile(UBOOT_PATTERN)
    banner = re.compile(rb"Hit SPACE in [0-9]+ seconds to stop autoboot\.")
    space_sent = False
    stop_deadline = min(deadline, time.monotonic() + 15.0)
    while time.monotonic() < stop_deadline:
        readable, _, _ = select.select([fd], [], [], 0.05)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            continue
        stream.write(chunk)
        stream.flush()
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        captured.extend(chunk)
        window = bytes(captured[-16384:])
        if not space_sent and banner.search(window):
            paced_write(fd, " ", 0.0)
            termios.tcdrain(fd)
            space_sent = True
        if prompt.search(window):
            return
    # A noisy VCP can lose the original prompt.  End any accumulated blank
    # command and request a fresh prompt after the autoboot window is over.
    os.write(fd, b"\r")
    wait_for(fd, stream, UBOOT_PATTERN, deadline)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--already-in-uboot", action="store_true")
    parser.add_argument("--wait-for-autoboot", action="store_true")
    parser.add_argument("--boot", action="store_true")
    parser.add_argument("--username", default="root")
    parser.add_argument("--password-env", default="SERIAL_CONSOLE_PASSWORD")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--character-delay-ms", type=float, default=20.0)
    args = parser.parse_args()

    if args.wait_for_autoboot and not args.already_in_uboot:
        parser.error("--wait-for-autoboot requires --already-in-uboot")

    if not args.device.startswith("/dev/") or not os.path.exists(args.device):
        parser.error("--device must name an existing exact /dev path")
    password = os.environ.get(args.password_env)
    if not args.already_in_uboot and password is None:
        parser.error(f"password environment variable {args.password_env} is unset")

    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)
    delay = args.character_delay_ms / 1000.0
    deadline = time.monotonic() + args.timeout

    with args.output.open("wb") as stream:
        if args.already_in_uboot:
            if args.wait_for_autoboot:
                stop_autoboot(fd, stream, deadline)
            else:
                paced_write(fd, "\r", delay)
                wait_for(fd, stream, UBOOT_PATTERN, deadline)
        else:
            paced_write(fd, "\r", delay)
            captured = bytearray()
            state = read_until(
                fd, stream, captured, (LOGIN_PATTERN, SHELL_PATTERN), deadline
            )
            if state is None:
                raise SystemExit("neither login nor shell prompt was observed")
            if state == 0:
                paced_write(fd, args.username + "\r", delay)
                wait_for(fd, stream, rb"Password: ", deadline)
                paced_write(fd, password + "\r", delay)
                wait_for(fd, stream, SHELL_PATTERN, deadline)

            paced_write(
                fd,
                "echo 40000400.timer > "
                "/sys/bus/platform/drivers/hiroic-rt-scope/unbind 2>/dev/null; "
                "reboot\r",
                delay,
            )
            wait_for(fd, stream, rb"Hit SPACE in 3 seconds to stop autoboot\.", deadline)
            paced_write(fd, " ", delay)
            wait_for(fd, stream, UBOOT_PATTERN, deadline)

        for command in args.command:
            paced_write(fd, command + "\r", delay)
            wait_for(fd, stream, UBOOT_PATTERN, deadline)

        if args.boot:
            paced_write(fd, "boot\r", delay)
            wait_for(fd, stream, LOGIN_PATTERN, deadline)

    os.close(fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
