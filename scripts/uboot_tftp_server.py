#!/usr/bin/env python3
"""Serve one exact file to U-Boot with a small, read-only TFTP server.

This intentionally implements only an octet-mode RRQ and exits after one
successful transfer.  It is useful on a benchmark-side host where installing
a persistent TFTP daemon would add unnecessary state.
"""

import argparse
import socket
import struct
import sys
from pathlib import Path


OP_RRQ = 1
OP_DATA = 3
OP_ACK = 4
OP_ERROR = 5
BLOCK_SIZE = 512


def error_packet(code: int, message: str) -> bytes:
    return struct.pack("!HH", OP_ERROR, code) + message.encode("ascii") + b"\0"


def parse_rrq(packet: bytes) -> tuple[str, str]:
    if len(packet) < 4 or struct.unpack("!H", packet[:2])[0] != OP_RRQ:
        raise ValueError("not an RRQ")
    fields = packet[2:].split(b"\0")
    if len(fields) < 3 or not fields[0] or not fields[1]:
        raise ValueError("malformed RRQ")
    return fields[0].decode("ascii"), fields[1].decode("ascii").lower()


def serve(bind: str, port: int, requested_name: str, path: Path, timeout: float) -> None:
    payload = path.read_bytes()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind((bind, port))
        print(
            f"ready: tftp://{bind}:{port}/{requested_name} "
            f"({len(payload)} bytes from {path})",
            flush=True,
        )
        while True:
            request, peer = listener.recvfrom(2048)
            try:
                name, mode = parse_rrq(request)
            except (ValueError, UnicodeDecodeError):
                continue
            if name != requested_name:
                listener.sendto(error_packet(1, "file not found"), peer)
                continue
            if mode != "octet":
                listener.sendto(error_packet(4, "octet mode required"), peer)
                continue
            break

    # RFC 1350 assigns a new transfer identifier (UDP port) to the data side.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as transfer:
        transfer.bind((bind, 0))
        transfer.settimeout(timeout)
        offset = 0
        block = 1
        while True:
            chunk = payload[offset : offset + BLOCK_SIZE]
            packet = struct.pack("!HH", OP_DATA, block) + chunk
            for attempt in range(6):
                transfer.sendto(packet, peer)
                try:
                    ack, ack_peer = transfer.recvfrom(2048)
                except TimeoutError:
                    continue
                if ack_peer != peer or len(ack) < 4:
                    continue
                opcode, ack_block = struct.unpack("!HH", ack[:4])
                if opcode == OP_ACK and ack_block == block:
                    break
            else:
                raise RuntimeError(f"no ACK for block {block} after 6 attempts")

            offset += len(chunk)
            if len(chunk) < BLOCK_SIZE:
                break
            block = (block + 1) & 0xFFFF

    print(f"complete: sent {offset} bytes to {peer[0]}:{peer[1]}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", required=True)
    parser.add_argument("--port", type=int, default=1069)
    parser.add_argument("--name", required=True)
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if "/" in args.name or args.name in ("", ".", ".."):
        parser.error("--name must be a single exact TFTP filename")
    if not args.file.is_file():
        parser.error("--file must name an existing regular file")
    try:
        serve(args.bind, args.port, args.name, args.file, args.timeout)
    except (OSError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
