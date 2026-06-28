#!/usr/bin/env python3
"""Minimal IQ server client for E2E verification."""

from __future__ import annotations

import argparse
import socket
import struct
import sys

REQUEST_MAGIC = 0x51495249
RESPONSE_MAGIC = 0x51535249
PROTOCOL_VERSION = 1


def build_request(targets: list[tuple[float, float, float, int]]) -> bytes:
    header = struct.pack("<IHH", REQUEST_MAGIC, PROTOCOL_VERSION, len(targets))
    body = b"".join(
        struct.pack("<fffB3x", az, el, dist, mode) for az, el, dist, mode in targets
    )
    return header + body


def parse_response(data: bytes) -> tuple[int, int, int, list[tuple[float, float]]]:
    if len(data) < 12:
        raise ValueError("response too short")
    magic, version, target_count, status = struct.unpack_from("<I H H I", data, 0)
    if magic != RESPONSE_MAGIC:
        raise ValueError(f"bad response magic: {magic:#010x}")
    offset = 12
    samples: list[tuple[float, float]] = []
    for _ in range(target_count):
        if status != 0:
            break
        # caller must know mode sizes; here we just read remaining floats
        while offset + 8 <= len(data):
            i, q = struct.unpack_from("<ff", data, offset)
            samples.append((i, q))
            offset += 8
        break
    return version, target_count, status, samples


def main() -> int:
    parser = argparse.ArgumentParser(description="iq-server test client")
    parser.add_argument("--host", default="192.168.0.33")
    parser.add_argument("--port", type=int, default=29171)
    parser.add_argument("--azimuth", type=float, default=0.0)
    parser.add_argument("--elevation", type=float, default=0.0)
    parser.add_argument("--distance", type=float, default=2.5)
    parser.add_argument("--mode", type=int, default=1, help="0=PerChirp,1=Average,2=MaxAbs")
    args = parser.parse_args()

    payload = build_request([(args.azimuth, args.elevation, args.distance, args.mode)])
    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.sendall(payload)
        response = sock.recv(65536)

    version, target_count, status, samples = parse_response(response)
    print(f"version={version} targets={target_count} status={status} samples={len(samples)}")
    for idx, (i, q) in enumerate(samples[:5]):
        print(f"  [{idx}] I={i:.6f} Q={q:.6f}")
    if len(samples) > 5:
        print(f"  ... {len(samples) - 5} more")
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
