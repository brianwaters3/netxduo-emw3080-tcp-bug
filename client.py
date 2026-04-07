#!/usr/bin/env python3
"""
Minimal client for the NetXDuo + EMW3080 TCP server reproducer.

Repeatedly connects to the MCU's TCP echo server, sends one byte,
reads the echo, disconnects, and repeats.  Tracks how many cycles
complete before the server stops responding.

Usage:
    python3 client.py <mcu-ip> [port]

Default port is 6000 (matches DEFAULT_PORT in ST's Nx_TCP_Echo_Server
sample).  The reproducer typically fails after 5-80 cycles -- either
the MCU hard-faults inside _nx_ip_packet_send (visible on the ST-Link
VCP if you've added a HardFault dump) or the MCU silently stops
accepting new connections.
"""

import socket
import sys
import time


def cycle(host: str, port: int, payload: bytes) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((host, port))
    s.sendall(payload)
    echoed = s.recv(len(payload))
    if echoed != payload:
        raise RuntimeError(f"echo mismatch: sent {payload!r} got {echoed!r}")
    s.shutdown(socket.SHUT_RDWR)
    s.close()


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.25"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6000
    payload = b"a"

    print(f"Hammering {host}:{port} ...")
    n = 0
    started = time.monotonic()
    try:
        while True:
            cycle(host, port, payload)
            n += 1
            if n % 10 == 0:
                rate = n / (time.monotonic() - started)
                print(f"  {n} cycles ok ({rate:.1f}/sec)")
    except (socket.timeout, ConnectionRefusedError, OSError) as e:
        elapsed = time.monotonic() - started
        print(f"FAIL after {n} successful cycles ({elapsed:.1f}s): "
              f"{type(e).__name__}: {e}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
