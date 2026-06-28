"""Minimal IQ server client (shared with test-iq)."""

from __future__ import annotations

import socket
import struct
from dataclasses import dataclass

REQUEST_MAGIC = 0x51495249
RESPONSE_MAGIC = 0x51535249
PROTOCOL_VERSION = 1

CHIRP_MODES = {
    0: ("PerChirp", 64),
    1: ("Average", 1),
    2: ("MaxAbs", 1),
}


@dataclass(frozen=True, slots=True)
class Target:
    azimuth_rad: float
    elevation_rad: float
    distance_m: float
    chirp_mode: int = 1

    @property
    def sample_count(self) -> int:
        return CHIRP_MODES[self.chirp_mode][1]


@dataclass(frozen=True, slots=True)
class IqSample:
    i: float
    q: float


def build_request(targets: list[Target]) -> bytes:
    header = struct.pack("<IHH", REQUEST_MAGIC, PROTOCOL_VERSION, len(targets))
    body = b"".join(
        struct.pack("<fffB3x", t.azimuth_rad, t.elevation_rad, t.distance_m, t.chirp_mode)
        for t in targets
    )
    return header + body


def _recv_all(sock: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    received = 0
    while received < size:
        chunk = sock.recv(size - received)
        if not chunk:
            raise ConnectionError("connection closed while receiving response")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def request_once(host: str, port: int, targets: list[Target], timeout: float) -> tuple[int, list[list[IqSample]]]:
    payload = build_request(targets)
    expected = 12 + sum(t.sample_count * 8 for t in targets)

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload)
        header = _recv_all(sock, 12)
        magic, _version, target_count, status = struct.unpack_from("<I H H I", header, 0)
        if magic != RESPONSE_MAGIC:
            raise ValueError(f"bad response magic: {magic:#010x}")
        if status != 0:
            return status, []

        body = _recv_all(sock, expected - 12)
        data = header + body

    offset = 12
    parsed: list[list[IqSample]] = []
    for target in targets[:target_count]:
        samples: list[IqSample] = []
        for _ in range(target.sample_count):
            i_val, q_val = struct.unpack_from("<ff", data, offset)
            samples.append(IqSample(i_val, q_val))
            offset += 8
        parsed.append(samples)
    return 0, parsed
