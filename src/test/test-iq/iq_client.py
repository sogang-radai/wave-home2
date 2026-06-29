"""IQ server protocol v1 and target parsing (shared by cli-tool and GUI)."""

from __future__ import annotations

import math
import socket
import struct
import subprocess
from dataclasses import dataclass

REQUEST_MAGIC = 0x51495249
RESPONSE_MAGIC = 0x51535249
PROTOCOL_VERSION = 1

REQUEST_TYPE_IQ = 0
REQUEST_TYPE_RDM = 1

REQUEST_HEADER_SIZE = 12
RESPONSE_HEADER_SIZE = 16
IQ_RESPONSE_INFO_SIZE = 4
RDM_RESPONSE_INFO_SIZE = 20
IQ_RESPONSE_PREFIX_SIZE = RESPONSE_HEADER_SIZE + IQ_RESPONSE_INFO_SIZE
RDM_RESPONSE_PREFIX_SIZE = RESPONSE_HEADER_SIZE + RDM_RESPONSE_INFO_SIZE

IQ_REQUEST_SIZE = 28
RDM_REQUEST_SIZE = 44

TILE_COUNT = 12
SUB_ANT_COUNT = 16

DEFAULT_HOST = "192.168.0.33"
DEFAULT_PORT = 29171
DEFAULT_TIMEOUT_S = 5.0
DEFAULT_POLL_INTERVAL_S = 0.05

RANGE_BIN_SIZE_M = 0.07156503945589066
VELOCITY_BIN_SIZE_MPS = 0.07242187857627869

CHIRP_MODE_ARRAY = 0
CHIRP_MODE_AVERAGE = 1
CHIRP_MODE_MAX_ABS = 2

CHIRP_MODES = {
    CHIRP_MODE_ARRAY: ("Array", 64),
    CHIRP_MODE_AVERAGE: ("Average", 1),
    CHIRP_MODE_MAX_ABS: ("MaxAbs", 1),
}

CHIRP_SELECT_ALL = 0
CHIRP_SELECT_MASK = 1

VA_SELECT_SINGLE = 0
VA_SELECT_MASK = 1
VA_SELECT_LIST = 2

VA_COMBINE_MULTI = 0
VA_COMBINE_AVERAGE = 1
VA_COMBINE_BEAMFORM = 2

# GUI alias (was VA_COMBINE_SINGLE in protocol v2 wire)
VA_COMBINE_SINGLE = VA_COMBINE_MULTI

VA_COMBINE_MODES = {
    VA_COMBINE_MULTI: "Single TX-RX",
    VA_COMBINE_AVERAGE: "Average all (192 VA)",
    VA_COMBINE_BEAMFORM: "Beamform (az/el)",
}

VA_MASK_ALL = tuple(0xFFFF for _ in range(TILE_COUNT))


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
    chirp_mode: int = CHIRP_MODE_AVERAGE

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
    chirp_mode: int = CHIRP_MODE_ARRAY
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
    cells: list[list[IqSample]]

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
    mode = int(parts[3]) if len(parts) == 4 else CHIRP_MODE_AVERAGE
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
    chirp_mode: int = CHIRP_MODE_AVERAGE,
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


def build_vital_range_targets(
    azimuth_deg: float,
    elevation_deg: float,
    center_distance_m: float,
    range_bin_m: float = RANGE_BIN_SIZE_M,
    half_width: int = 2,
    chirp_mode: int = CHIRP_MODE_MAX_ABS,
) -> list[tuple[int, Target]]:
    """Return (range_bin_index, Target) for center ± half_width bins (TI vital signs)."""
    az = math.radians(azimuth_deg)
    el = math.radians(elevation_deg)
    center_bin = round(center_distance_m / range_bin_m)
    out: list[tuple[int, Target]] = []
    for offset in range(-half_width, half_width + 1):
        rb = center_bin + offset
        distance = rb * range_bin_m
        out.append((rb, Target(az, el, distance, chirp_mode=chirp_mode)))
    return out


def _dist_key(distance_m: float) -> float:
    return round(distance_m / RANGE_BIN_SIZE_M) * RANGE_BIN_SIZE_M


def merge_profile_with_vital_targets(
    profile_targets: list[Target],
    pick_range_m: float,
    half_width: int = 2,
    vital_chirp_mode: int = CHIRP_MODE_MAX_ABS,
) -> tuple[list[Target], list[tuple[int, int]]]:
    """Append vital-sign bins to profile request; return merged targets and (rb, response_index)."""
    if not profile_targets:
        return profile_targets, []

    az_deg = math.degrees(profile_targets[0].azimuth_rad)
    el_deg = math.degrees(profile_targets[0].elevation_rad)
    vital_pairs = build_vital_range_targets(
        az_deg,
        el_deg,
        pick_range_m,
        RANGE_BIN_SIZE_M,
        half_width,
        vital_chirp_mode,
    )

    merged: list[Target] = list(profile_targets)
    index_by_dist: dict[float, int] = {_dist_key(t.distance_m): i for i, t in enumerate(merged)}
    vital_indices: list[tuple[int, int]] = []

    for rb, vital_target in vital_pairs:
        key = _dist_key(vital_target.distance_m)
        if key in index_by_dist:
            idx = index_by_dist[key]
            existing = merged[idx]
            if existing.chirp_mode != vital_chirp_mode:
                merged[idx] = Target(
                    existing.azimuth_rad,
                    existing.elevation_rad,
                    existing.distance_m,
                    vital_chirp_mode,
                )
            vital_indices.append((rb, idx))
        else:
            idx = len(merged)
            merged.append(vital_target)
            index_by_dist[key] = idx
            vital_indices.append((rb, idx))

    return merged, vital_indices


def resolve_vital_response_indices(
    merged_targets: list[Target],
    profile_targets: list[Target],
    pick_range_m: float,
    half_width: int = 2,
    vital_chirp_mode: int = CHIRP_MODE_AVERAGE,
) -> list[tuple[int, int]]:
    """Map vital range bins to response indices by distance (robust to request/response reorder)."""
    if not profile_targets or not merged_targets:
        return []

    az_deg = math.degrees(profile_targets[0].azimuth_rad)
    el_deg = math.degrees(profile_targets[0].elevation_rad)
    vital_pairs = build_vital_range_targets(
        az_deg,
        el_deg,
        pick_range_m,
        RANGE_BIN_SIZE_M,
        half_width,
        vital_chirp_mode,
    )
    dist_to_idx: dict[float, int] = {_dist_key(t.distance_m): i for i, t in enumerate(merged_targets)}
    out: list[tuple[int, int]] = []
    for rb, vital_target in vital_pairs:
        key = _dist_key(vital_target.distance_m)
        if key in dist_to_idx:
            out.append((rb, dist_to_idx[key]))
    return out


def snap_vital_pick_distance(profile_targets: list[Target], click_distance_m: float) -> float:
    """Snap click to profile grid, then to firmware range-bin grid."""
    if not profile_targets:
        rb = round(click_distance_m / RANGE_BIN_SIZE_M)
        return rb * RANGE_BIN_SIZE_M
    profile_m = min(profile_targets, key=lambda t: abs(t.distance_m - click_distance_m)).distance_m
    rb = round(profile_m / RANGE_BIN_SIZE_M)
    return rb * RANGE_BIN_SIZE_M


def _common_chirp_mode(targets: list[Target]) -> int:
    mode = targets[0].chirp_mode
    for target in targets[1:]:
        if target.chirp_mode != mode:
            raise ValueError("all targets must share the same chirp_mode in protocol v1")
    return mode


def _va_wire_fields(
    va_combine_mode: int,
    tile: int,
    sub_ant: int,
) -> tuple[int, int, int, tuple[int, ...] | None]:
    if va_combine_mode == VA_COMBINE_MULTI:
        return VA_SELECT_SINGLE, tile, sub_ant, None
    if va_combine_mode == VA_COMBINE_AVERAGE:
        return VA_SELECT_MASK, 0, 0, VA_MASK_ALL
    return VA_SELECT_SINGLE, tile, sub_ant, None


def _pack_va_tail(
    va_select_mode: int,
    va_masks: tuple[int, ...] | None,
    va_list: list[tuple[int, int]],
) -> bytes:
    tail = b""
    if va_select_mode == VA_SELECT_MASK and va_masks is not None:
        tail += struct.pack(f"<{TILE_COUNT}H", *va_masks)
    elif va_select_mode == VA_SELECT_LIST:
        tail += struct.pack("<H", len(va_list))
        for tile, sub_ant in va_list:
            tail += struct.pack("<BB", tile, sub_ant)
    return tail


def build_request(
    targets: list[Target],
    va_combine_mode: int = VA_COMBINE_BEAMFORM,
    tile: int = 0,
    sub_ant: int = 0,
    chirp_select_mode: int = CHIRP_SELECT_ALL,
    chirp_mask: int | None = None,
) -> bytes:
    if not targets:
        raise ValueError("no targets to request")

    chirp_mode = _common_chirp_mode(targets)
    va_select_mode, req_tile, req_sub_ant, va_masks = _va_wire_fields(
        va_combine_mode,
        tile,
        sub_ant,
    )

    body = struct.pack(
        "<B3xIIHHBBHff",
        REQUEST_TYPE_IQ,
        chirp_mode,
        chirp_select_mode,
        va_select_mode,
        va_combine_mode,
        req_tile,
        req_sub_ant,
        len(targets),
        targets[0].azimuth_rad,
        targets[0].elevation_rad,
    )
    body += struct.pack(f"<{len(targets)}f", *(t.distance_m for t in targets))

    if chirp_select_mode == CHIRP_SELECT_MASK:
        if chirp_mask is None:
            raise ValueError("chirp_mask required when chirp_select_mode is Mask")
        body += struct.pack("<Q", chirp_mask)

    body += _pack_va_tail(va_select_mode, va_masks, [])

    header = struct.pack(
        "<III",
        REQUEST_MAGIC,
        PROTOCOL_VERSION,
        len(body),
    )
    return header + body


def build_rdm_request(spec: RdmSpec) -> bytes:
    va_select_mode, req_tile, req_sub_ant, va_masks = _va_wire_fields(
        spec.va_combine_mode,
        spec.tile,
        spec.sub_ant,
    )

    body = struct.pack(
        "<B3xIIHHBBHffffff",
        REQUEST_TYPE_RDM,
        spec.chirp_mode,
        CHIRP_SELECT_ALL,
        va_select_mode,
        spec.va_combine_mode,
        req_tile,
        req_sub_ant,
        0,
        spec.azimuth_rad,
        spec.elevation_rad,
        spec.distance_min_m,
        spec.distance_max_m,
        spec.velocity_min_mps,
        spec.velocity_max_mps,
    )
    body += _pack_va_tail(va_select_mode, va_masks, [])

    header = struct.pack(
        "<III",
        REQUEST_MAGIC,
        PROTOCOL_VERSION,
        len(body),
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
            va_combine_mode=va_combine_mode,
            tile=tile,
            sub_ant=sub_ant,
        )
        chirp_mode = _common_chirp_mode(targets)
        try:
            self._sock.sendall(payload)
            prefix = recv_all(self._sock, IQ_RESPONSE_PREFIX_SIZE)
            response = parse_response_prefix(prefix, targets, chirp_mode)
            if response.status != 0:
                return response
            body_size = expected_response_bytes(targets, chirp_mode) - IQ_RESPONSE_PREFIX_SIZE
            body = recv_all(self._sock, body_size) if body_size else b""
            return parse_response(prefix + body, targets, chirp_mode)
        except OSError:
            self.close()
            raise

    def request_rdm(self, spec: RdmSpec) -> RdmResponse:
        self._ensure(self._host, self._port, self._timeout)
        assert self._sock is not None
        payload = build_rdm_request(spec)
        try:
            self._sock.sendall(payload)
            prefix = recv_all(self._sock, RDM_RESPONSE_PREFIX_SIZE)
            response = parse_rdm_response_prefix(prefix)
            if response.status != 0:
                return response
            body_size = response.range_count * response.doppler_count * 8
            body = recv_all(self._sock, body_size) if body_size else b""
            return parse_rdm_response(prefix + body)
        except OSError:
            self.close()
            raise


def expected_response_bytes(targets: list[Target], chirp_mode: int | None = None) -> int:
    mode = chirp_mode if chirp_mode is not None else _common_chirp_mode(targets)
    samples_per_target = CHIRP_MODES[mode][1]
    return IQ_RESPONSE_PREFIX_SIZE + len(targets) * samples_per_target * 8


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


def parse_response_prefix(
    prefix: bytes,
    request_targets: list[Target],
    chirp_mode: int,
) -> IqResponse:
    if len(prefix) < IQ_RESPONSE_PREFIX_SIZE:
        raise ValueError(f"response too short: {len(prefix)} bytes")

    magic, version, _payload_size, status = struct.unpack_from("<IIII", prefix, 0)
    if magic != RESPONSE_MAGIC:
        raise ValueError(f"bad response magic: {magic:#010x}")

    target_count, _reserved = struct.unpack_from("<HH", prefix, RESPONSE_HEADER_SIZE)
    if status != 0:
        return IqResponse(version, target_count, status, [])

    return IqResponse(version, target_count, status, [])


def parse_response(
    data: bytes,
    request_targets: list[Target],
    chirp_mode: int | None = None,
) -> IqResponse:
    mode = chirp_mode if chirp_mode is not None else _common_chirp_mode(request_targets)
    prefix = parse_response_prefix(data, request_targets, mode)
    if prefix.status != 0:
        return prefix

    expected = expected_response_bytes(request_targets, mode)
    if len(data) != expected:
        raise ValueError(f"unexpected payload size: got {len(data)}, expected {expected}")

    samples_per_target = CHIRP_MODES[mode][1]
    offset = IQ_RESPONSE_PREFIX_SIZE
    parsed_targets: list[list[IqSample]] = []
    for _ in range(prefix.target_count):
        samples: list[IqSample] = []
        for _ in range(samples_per_target):
            i_val, q_val = struct.unpack_from("<ff", data, offset)
            samples.append(IqSample(i_val, q_val))
            offset += 8
        parsed_targets.append(samples)

    return IqResponse(prefix.version, prefix.target_count, prefix.status, parsed_targets)


def parse_rdm_response_prefix(prefix: bytes) -> RdmResponse:
    if len(prefix) < RDM_RESPONSE_PREFIX_SIZE:
        raise ValueError("RDM response too short")

    magic, version, _payload_size, status = struct.unpack_from("<IIII", prefix, 0)
    if magic != RESPONSE_MAGIC:
        raise ValueError(f"bad response magic: {magic:#010x}")

    (
        range_count,
        doppler_count,
        range_min_m,
        range_step_m,
        velocity_min_mps,
        velocity_step_mps,
    ) = struct.unpack_from("<HHffff", prefix, RESPONSE_HEADER_SIZE)

    return RdmResponse(
        version,
        status,
        range_count,
        doppler_count,
        range_min_m,
        range_step_m,
        velocity_min_mps,
        velocity_step_mps,
        [],
    )


def parse_rdm_response(data: bytes) -> RdmResponse:
    prefix = parse_rdm_response_prefix(data[:RDM_RESPONSE_PREFIX_SIZE])
    if prefix.status != 0:
        return prefix

    cell_count = prefix.range_count * prefix.doppler_count
    expected = RDM_RESPONSE_PREFIX_SIZE + cell_count * 8
    if len(data) != expected:
        raise ValueError(f"RDM payload size mismatch: got {len(data)}, expected {expected}")

    flat: list[IqSample] = []
    offset = RDM_RESPONSE_PREFIX_SIZE
    for _ in range(cell_count):
        i_val, q_val = struct.unpack_from("<ff", data, offset)
        flat.append(IqSample(i_val, q_val))
        offset += 8

    cells: list[list[IqSample]] = []
    idx = 0
    for _ in range(prefix.range_count):
        row = flat[idx : idx + prefix.doppler_count]
        cells.append(row)
        idx += prefix.doppler_count

    return RdmResponse(
        prefix.version,
        prefix.status,
        prefix.range_count,
        prefix.doppler_count,
        prefix.range_min_m,
        prefix.range_step_m,
        prefix.velocity_min_mps,
        prefix.velocity_step_mps,
        cells,
    )


def request_once(
    host: str,
    port: int,
    targets: list[Target],
    timeout: float = DEFAULT_TIMEOUT_S,
    va_combine_mode: int = VA_COMBINE_BEAMFORM,
    tile: int = 0,
    sub_ant: int = 0,
) -> IqResponse:
    if not targets:
        raise ValueError("no targets to request")

    chirp_mode = _common_chirp_mode(targets)
    payload = build_request(
        targets,
        va_combine_mode=va_combine_mode,
        tile=tile,
        sub_ant=sub_ant,
    )
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload)
        prefix = recv_all(sock, IQ_RESPONSE_PREFIX_SIZE)
        response = parse_response_prefix(prefix, targets, chirp_mode)
        if response.status != 0:
            return response
        body_size = expected_response_bytes(targets, chirp_mode) - IQ_RESPONSE_PREFIX_SIZE
        body = recv_all(sock, body_size) if body_size else b""
        data = prefix + body
    return parse_response(data, targets, chirp_mode)


def request_rdm_once(
    host: str,
    port: int,
    spec: RdmSpec,
    timeout: float = DEFAULT_TIMEOUT_S,
) -> RdmResponse:
    payload = build_rdm_request(spec)
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload)
        prefix = recv_all(sock, RDM_RESPONSE_PREFIX_SIZE)
        response = parse_rdm_response_prefix(prefix)
        if response.status != 0:
            return response
        body_size = response.range_count * response.doppler_count * 8
        body = recv_all(sock, body_size) if body_size else b""
        data = prefix + body
    return parse_rdm_response(data)
