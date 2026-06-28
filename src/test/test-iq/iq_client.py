"""IQ server protocol and target parsing (shared by cli-tool and GUI)."""

from __future__ import annotations

import math
import socket
import struct
from dataclasses import dataclass

REQUEST_MAGIC = 0x51495249
RESPONSE_MAGIC = 0x51535249
PROTOCOL_VERSION = 1
PROTOCOL_VERSION2 = 2

REQUEST_TYPE_TARGET_IQ = 0
REQUEST_TYPE_RDM = 1

DEFAULT_HOST = "192.168.0.33"
DEFAULT_PORT = 29171
DEFAULT_TIMEOUT_S = 5.0
DEFAULT_POLL_INTERVAL_S = 0.05

# Firmware (r4fn.elf.c FUN_0040d6f0)
RANGE_BIN_SIZE_M = 0.07156503945589066
VELOCITY_BIN_SIZE_MPS = 0.07242187857627869

CHIRP_MODES = {
    0: ("PerChirp", 64),
    1: ("Average", 1),
    2: ("MaxAbs", 1),
    3: ("FirstChirp", 1),
}

VA_COMBINE_AVERAGE = 0
VA_COMBINE_SINGLE = 1
VA_COMBINE_BEAMFORM = 2

VA_COMBINE_MODES = {
    VA_COMBINE_AVERAGE: "Average all (192 VA)",
    VA_COMBINE_SINGLE: "Single TX-RX",
    VA_COMBINE_BEAMFORM: "Beamform (az/el)",
}

# Target IQ v2: RequestHeaderV2.reserved wire values (0 = legacy beamform).
VA_WIRE_LEGACY_BEAMFORM = 0
VA_WIRE_AVERAGE = 1
VA_WIRE_SINGLE = 2
VA_WIRE_BEAMFORM = 3

VA_TO_WIRE = {
    VA_COMBINE_AVERAGE: VA_WIRE_AVERAGE,
    VA_COMBINE_SINGLE: VA_WIRE_SINGLE,
    VA_COMBINE_BEAMFORM: VA_WIRE_BEAMFORM,
}


def tx_label(tile: int) -> str:
    return f"S{tile // 3 + 1}-TX{tile % 3 + 1}"


def rx_label(sub_ant: int) -> str:
    return f"S{sub_ant // 4 + 1}-RX{sub_ant % 4 + 1}"


def va_label(tile: int, sub_ant: int) -> str:
    return f"{tx_label(tile)} + {rx_label(sub_ant)}"


@dataclass(frozen=True, slots=True)
class Target:
    azimuth_rad: float
    elevation_rad: float
    distance_m: float
    chirp_mode: int = 1

    @property
    def mode_name(self) -> str:
        return CHIRP_MODES[self.chirp_mode][0]

    @property
    def sample_count(self) -> int:
        return CHIRP_MODES[self.chirp_mode][1]


@dataclass(frozen=True, slots=True)
class RdmSpec:
    azimuth_rad: float
    elevation_rad: float
    distance_min_m: float
    distance_max_m: float
    velocity_min_mps: float
    velocity_max_mps: float
    chirp_mode: int = 0
    va_combine_mode: int = VA_COMBINE_AVERAGE
    tile: int = 0
    sub_ant: int = 0


@dataclass(frozen=True, slots=True)
class IqSample:
    i: float
    q: float

    @property
    def magnitude(self) -> float:
        return math.hypot(self.i, self.q)

    @property
    def phase_deg(self) -> float:
        return math.degrees(math.atan2(self.q, self.i))

    @property
    def phase_rad(self) -> float:
        return math.atan2(self.q, self.i)


@dataclass(frozen=True, slots=True)
class IqResponse:
    version: int
    target_count: int
    status: int
    targets: list[list[IqSample]]


@dataclass(frozen=True, slots=True)
class RdmResponse:
    version: int
    status: int
    range_count: int
    doppler_count: int
    range_min_m: float
    range_step_m: float
    velocity_min_mps: float
    velocity_step_mps: float
    cells: list[list[IqSample]]  # [range_idx][doppler_idx]

    @property
    def distances_m(self) -> list[float]:
        return [self.range_min_m + i * self.range_step_m for i in range(self.range_count)]

    @property
    def velocities_mps(self) -> list[float]:
        if self.doppler_count <= 1:
            return [self.velocity_min_mps]
        return [self.velocity_min_mps + i * self.velocity_step_mps for i in range(self.doppler_count)]

    def magnitude_grid(self) -> list[list[float]]:
        return [[s.magnitude for s in row] for row in self.cells]

    def phase_grid(self) -> list[list[float]]:
        return [[s.phase_rad for s in row] for row in self.cells]


def parse_target_line(text: str, source: str) -> Target:
    parts = [p.strip() for p in text.split(",")]
    if len(parts) not in (3, 4):
        raise ValueError(f"{source}: expected az,el,dist[,mode], got: {text!r}")
    az, el, dist = float(parts[0]), float(parts[1]), float(parts[2])
    mode = int(parts[3]) if len(parts) == 4 else 1
    if mode not in CHIRP_MODES:
        raise ValueError(f"{source}: invalid chirp mode {mode}")
    return Target(az, el, dist, mode)


def parse_targets_text(text: str) -> tuple[list[Target], list[str]]:
    targets: list[Target] = []
    errors: list[str] = []
    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            targets.append(parse_target_line(line, f"line {line_no}"))
        except ValueError as exc:
            errors.append(str(exc))
    return targets, errors


def build_range_profile_targets(
    azimuth_deg: float,
    elevation_deg: float,
    dist_min_m: float,
    dist_max_m: float,
    step_m: float,
    chirp_mode: int = 1,
    pad_bins: int = 2,
) -> tuple[list[Target], float, float]:
    """Return targets plus fixed plot distance bounds (user range ± pad)."""
    if dist_max_m < dist_min_m:
        dist_min_m, dist_max_m = dist_max_m, dist_min_m
    if step_m <= 0:
        raise ValueError("step must be > 0")
    if chirp_mode not in CHIRP_MODES:
        raise ValueError(f"invalid chirp mode {chirp_mode}")

    pad_m = pad_bins * step_m
    plot_min = max(0.0, dist_min_m - pad_m)
    plot_max = dist_max_m + pad_m

    az = math.radians(azimuth_deg)
    el = math.radians(elevation_deg)
    targets: list[Target] = []
    d = plot_min
    while d <= plot_max + step_m * 0.5:
        if d > plot_max + 1e-9 and targets:
            break
        targets.append(Target(az, el, min(d, plot_max), chirp_mode))
        d += step_m
    if not targets:
        targets.append(Target(az, el, plot_min, chirp_mode))
    return targets, plot_min, plot_max


def build_request(
    targets: list[Target],
    use_v2: bool = False,
    va_combine_mode: int = VA_COMBINE_BEAMFORM,
    tile: int = 0,
    sub_ant: int = 0,
) -> bytes:
    if use_v2:
        va_wire = VA_TO_WIRE.get(va_combine_mode, VA_WIRE_BEAMFORM)
        header = struct.pack(
            "<IHBB",
            REQUEST_MAGIC,
            PROTOCOL_VERSION2,
            REQUEST_TYPE_TARGET_IQ,
            va_wire,
        )
        body = struct.pack("<HBB", len(targets), tile, sub_ant)
        body += b"".join(
            struct.pack("<fffB3x", t.azimuth_rad, t.elevation_rad, t.distance_m, t.chirp_mode)
            for t in targets
        )
        return header + body

    header = struct.pack("<IHH", REQUEST_MAGIC, PROTOCOL_VERSION, len(targets))
    body = b"".join(
        struct.pack("<fffB3x", t.azimuth_rad, t.elevation_rad, t.distance_m, t.chirp_mode)
        for t in targets
    )
    return header + body


def build_rdm_request(spec: RdmSpec) -> bytes:
    header = struct.pack(
        "<IHBB",
        REQUEST_MAGIC,
        PROTOCOL_VERSION2,
        REQUEST_TYPE_RDM,
        0,
    )
    body = struct.pack(
        "<ffffffBBBB",
        spec.azimuth_rad,
        spec.elevation_rad,
        spec.distance_min_m,
        spec.distance_max_m,
        spec.velocity_min_mps,
        spec.velocity_max_mps,
        spec.chirp_mode,
        spec.va_combine_mode,
        spec.tile,
        spec.sub_ant,
    )
    return header + body


class IqSession:
    """Persistent TCP session to iq-server (avoids per-frame connect overhead)."""

    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._host = ""
        self._port = 0
        self._timeout = DEFAULT_TIMEOUT_S

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def open(self, host: str, port: int, timeout: float = DEFAULT_TIMEOUT_S) -> None:
        self.close()
        self._host = host
        self._port = port
        self._timeout = timeout
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _ensure(self, host: str, port: int, timeout: float) -> None:
        if self._sock is None or host != self._host or port != self._port:
            self.open(host, port, timeout)
        else:
            self._timeout = timeout
            self._sock.settimeout(timeout)

    def request_iq(
        self,
        targets: list[Target],
        use_v2: bool = True,
        va_combine_mode: int = VA_COMBINE_BEAMFORM,
        tile: int = 0,
        sub_ant: int = 0,
    ) -> IqResponse:
        if not targets:
            raise ValueError("no targets to request")
        self._ensure(self._host, self._port, self._timeout)
        assert self._sock is not None
        payload = build_request(
            targets,
            use_v2=use_v2,
            va_combine_mode=va_combine_mode,
            tile=tile,
            sub_ant=sub_ant,
        )
        try:
            self._sock.sendall(payload)
            header = recv_all(self._sock, 12)
            _, version, target_count, status = struct.unpack_from("<I H H I", header, 0)
            if status != 0:
                return IqResponse(version, target_count, status, [])
            body = recv_all(self._sock, expected_response_bytes(targets) - 12)
            return parse_response(header + body, targets)
        except OSError:
            self.close()
            raise

    def request_rdm(self, spec: RdmSpec) -> RdmResponse:
        self._ensure(self._host, self._port, self._timeout)
        assert self._sock is not None
        payload = build_rdm_request(spec)
        try:
            self._sock.sendall(payload)
            header = recv_all(self._sock, 32)
            _, version, _, status, range_count, doppler_count, *_rest = struct.unpack_from(
                "<I H H I H H f f f f", header, 0
            )
            if status != 0:
                return RdmResponse(version, status, 0, 0, 0.0, 0.0, 0.0, 0.0, [])
            body_size = range_count * doppler_count * 8
            body = recv_all(self._sock, body_size) if body_size else b""
            return parse_rdm_response(header + body)
        except OSError:
            self.close()
            raise


def expected_response_bytes(targets: list[Target]) -> int:
    return 12 + sum(t.sample_count * 8 for t in targets)


def recv_all(sock: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    received = 0
    while received < size:
        chunk = sock.recv(size - received)
        if not chunk:
            raise ConnectionError("connection closed while receiving response")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def parse_response(data: bytes, request_targets: list[Target]) -> IqResponse:
    if len(data) < 12:
        raise ValueError(f"response too short: {len(data)} bytes")

    magic, version, target_count, status = struct.unpack_from("<I H H I", data, 0)
    if magic != RESPONSE_MAGIC:
        raise ValueError(f"bad response magic: {magic:#010x}")

    expected = expected_response_bytes(request_targets)
    if status == 0 and len(data) != expected:
        raise ValueError(f"unexpected payload size: got {len(data)}, expected {expected}")

    offset = 12
    parsed_targets: list[list[IqSample]] = []
    for target in request_targets[:target_count]:
        samples: list[IqSample] = []
        for _ in range(target.sample_count):
            i_val, q_val = struct.unpack_from("<ff", data, offset)
            samples.append(IqSample(i_val, q_val))
            offset += 8
        parsed_targets.append(samples)

    return IqResponse(version, target_count, status, parsed_targets)


def parse_rdm_response(data: bytes) -> RdmResponse:
    header_size = 32
    if len(data) < header_size:
        raise ValueError("RDM response too short")

    (
        magic,
        version,
        request_type,
        status,
        range_count,
        doppler_count,
        range_min_m,
        range_step_m,
        velocity_min_mps,
        velocity_step_mps,
    ) = struct.unpack_from("<I H H I H H f f f f", data, 0)

    if magic != RESPONSE_MAGIC:
        raise ValueError(f"bad response magic: {magic:#010x}")
    if status != 0:
        return RdmResponse(
            version,
            status,
            0,
            0,
            0.0,
            0.0,
            0.0,
            0.0,
            [],
        )

    cell_count = range_count * doppler_count
    expected = header_size + cell_count * 8
    if len(data) != expected:
        raise ValueError(f"RDM payload size mismatch: got {len(data)}, expected {expected}")

    flat: list[IqSample] = []
    offset = header_size
    for _ in range(cell_count):
        i_val, q_val = struct.unpack_from("<ff", data, offset)
        flat.append(IqSample(i_val, q_val))
        offset += 8

    cells: list[list[IqSample]] = []
    idx = 0
    for _ in range(range_count):
        row = flat[idx : idx + doppler_count]
        cells.append(row)
        idx += doppler_count

    return RdmResponse(
        version,
        status,
        range_count,
        doppler_count,
        range_min_m,
        range_step_m,
        velocity_min_mps,
        velocity_step_mps,
        cells,
    )


def request_once(
    host: str,
    port: int,
    targets: list[Target],
    timeout: float = DEFAULT_TIMEOUT_S,
    use_v2: bool = False,
) -> IqResponse:
    if not targets:
        raise ValueError("no targets to request")

    payload = build_request(targets, use_v2=use_v2)
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload)
        header = recv_all(sock, 12)
        _, version, target_count, status = struct.unpack_from("<I H H I", header, 0)
        if status != 0:
            return IqResponse(version, target_count, status, [])
        body = recv_all(sock, expected_response_bytes(targets) - 12)
        data = header + body
    return parse_response(data, targets)


def request_rdm_once(
    host: str,
    port: int,
    spec: RdmSpec,
    timeout: float = DEFAULT_TIMEOUT_S,
) -> RdmResponse:
    payload = build_rdm_request(spec)
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload)
        header = recv_all(sock, 32)
        _, version, _, status, range_count, doppler_count, *_rest = struct.unpack_from("<I H H I H H f f f f", header, 0)
        if status != 0:
            return RdmResponse(version, status, 0, 0, 0.0, 0.0, 0.0, 0.0, [])
        body_size = range_count * doppler_count * 8
        body = recv_all(sock, body_size) if body_size else b""
        data = header + body
    return parse_rdm_response(data)
