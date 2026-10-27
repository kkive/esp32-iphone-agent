#!/usr/bin/env python3
"""Smoke-test the ESP32 Agent authenticated Wi-Fi control service."""

import argparse
import json
import socket
import struct
import time

MAGIC = b"IAW1"
HEADER = struct.Struct("<4sBBHIII")
HELLO, HELLO_REPLY, STATUS, COMMAND, REPLY, PING, PONG, ERROR = 1, 2, 5, 6, 7, 8, 9, 10


def send_frame(sock: socket.socket, kind: int, seq: int, payload: bytes = b"") -> None:
    sock.sendall(HEADER.pack(MAGIC, kind, 0, HEADER.size, len(payload), seq, 0) + payload)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    while size:
        data = sock.recv(size)
        if not data:
            raise ConnectionError("board closed the connection")
        chunks.append(data)
        size -= len(data)
    return b"".join(chunks)


def recv_frame(sock: socket.socket) -> tuple[int, int, int, bytes]:
    magic, kind, flags, header_len, length, seq, board_ms = HEADER.unpack(recv_exact(sock, HEADER.size))
    if magic != MAGIC or header_len != HEADER.size or length > 65536:
        raise ValueError("bad frame header")
    return kind, seq, board_ms, recv_exact(sock, length)


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 Agent Wi-Fi transport smoke test")
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token", required=True, help="token from the encrypted BLE network configuration")
    parser.add_argument("--command", default="ping", help="board command to exercise after authentication")
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.settimeout(5)
        send_frame(sock, HELLO, 1, args.token.encode())
        kind, _, _, payload = recv_frame(sock)
        if kind != HELLO_REPLY or payload != b"ok":
            raise RuntimeError(f"authentication failed: {payload.decode(errors='replace')}")
        print("authenticated")

        started = time.perf_counter()
        send_frame(sock, PING, 2, b"smoke")
        while True:
            kind, seq, board_ms, payload = recv_frame(sock)
            if kind == STATUS:
                try:
                    print("status", json.dumps(json.loads(payload), ensure_ascii=False))
                except json.JSONDecodeError:
                    print("status", payload.decode(errors="replace"))
            elif kind == PONG and seq == 2:
                print(f"pong rtt={1000 * (time.perf_counter() - started):.1f}ms board={board_ms}ms")
                break
            elif kind == ERROR:
                raise RuntimeError(payload.decode(errors="replace"))

        send_frame(sock, COMMAND, 3, args.command.encode())
        while True:
            kind, seq, _, payload = recv_frame(sock)
            if kind == REPLY and seq == 3:
                print("reply", payload.decode(errors="replace"))
                break
            if kind == STATUS:
                print("status", payload.decode(errors="replace"))


if __name__ == "__main__":
    main()

