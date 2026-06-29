#!/usr/bin/env python3
"""Minimal IQ server client for E2E verification (protocol v1)."""

from __future__ import annotations

import argparse
import socket
import struct
import sys

REQUEST_MAGIC = 0x51495249
RESPONSE_MAGIC = 0x51535249
PROTOCOL_VERSION = 1

REQUEST_TYPE_IQ = 0
CHIRP_MODE_ARRAY = 0
CHIRP_MODE_AVERAGE = 1
CHIRP_MODE_MAX_ABS = 2

VA_COMBINE_BEAMFORM = 2
VA_SELECT_SINGLE = 0

CHIRP_SAMPLES = {
    CHIRP_MODE_ARRAY: 64,
    CHIRP_MODE_AVERAGE: 1,
    CHIRP_MODE_MAX_ABS: 1,
}


def build_request(
    azimuth_rad: float,
    elevation_rad: float,
    distance_m: float,
    chirp_mode: int,
) -> bytes:
    body = struct.pack(
        "<B3xIIHHBBHff",
        REQUEST_TYPE_IQ,
        chirp_mode,
        0,
        VA_SELECT_SINGLE,
        VA_COMBINE_BEAMFORM,
        0,
        0,
        1,
        azimuth_rad,
        elevation_rad,
    )
    body += struct.pack("<f", distance_m)
    header = struct.pack("<III", REQUEST_MAGIC, PROTOCOL_VERSION, len(body))
    return header + body


def recv_all(sock: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    received = 0
    while received < size:
        chunk = sock.recv(size - received)
        if not chunk:
            raise ConnectionError("connection closed")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def parse_response(data: bytes, chirp_mode: int) -> tuple[int, int, int, list[tuple[float, float]]]:
    if len(data) < 20:
        raise ValueError("response too short")
    magic, version, _payload_size, status = struct.unpack_from("<IIII", data, 0)
    if magic != RESPONSE_MAGIC:
        raise ValueError(f"bad response magic: {magic:#010x}")
    target_count, _reserved = struct.unpack_from("<HH", data, 16)
    samples: list[tuple[float, float]] = []
    if status == 0:
        sample_count = CHIRP_SAMPLES[chirp_mode]
        offset = 20
        for _ in range(target_count):
            for _ in range(sample_count):
                i_val, q_val = struct.unpack_from("<ff", data, offset)
                samples.append((i_val, q_val))
                offset += 8
    return version, target_count, status, samples


def main() -> int:
    parser = argparse.ArgumentParser(description="iq-server test client")
    parser.add_argument("--host", default="192.168.0.33")
    parser.add_argument("--port", type=int, default=29171)
    parser.add_argument("--azimuth", type=float, default=0.0)
    parser.add_argument("--elevation", type=float, default=0.0)
    parser.add_argument("--distance", type=float, default=2.5)
    parser.add_argument("--mode", type=int, default=1, help="0=Array,1=Average,2=MaxAbs")
    args = parser.parse_args()

    if args.mode not in CHIRP_SAMPLES:
        print(f"invalid mode {args.mode}", file=sys.stderr)
        return 2

    payload = build_request(args.azimuth, args.elevation, args.distance, args.mode)
    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.sendall(payload)
        prefix = recv_all(sock, 20)
        version, target_count, status, _ = parse_response(prefix, args.mode)
        if status == 0:
            body_size = target_count * CHIRP_SAMPLES[args.mode] * 8
            body = recv_all(sock, body_size) if body_size else b""
            version, target_count, status, samples = parse_response(prefix + body, args.mode)
        else:
            samples = []

    print(f"version={version} targets={target_count} status={status} samples={len(samples)}")
    for idx, (i, q) in enumerate(samples[:5]):
        print(f"  [{idx}] I={i:.6f} Q={q:.6f}")
    if len(samples) > 5:
        print(f"  ... {len(samples) - 5} more")
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
