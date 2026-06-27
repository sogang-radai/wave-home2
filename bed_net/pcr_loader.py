from __future__ import annotations
import ctypes
import struct
import os
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple, Optional, Dict

PCR_MAGIC = b"PCR1"
PCR_VERSION = b"1000"
INTEGRITY_MAGIC = 0x0807060504030201

# ============================================================================
# Dataclasses
# ============================================================================
@dataclass(slots=True)
class SensorSpec:
    hfovDeg: float
    vfovDeg: float
    sensorWidth: float
    sensorHeight: float
    posX: float
    posY: float
    posZ: float
    yawDeg: float
    pitchDeg: float
    rangeMinX: float
    rangeMinY: float
    rangeMinZ: float
    rangeMaxX: float
    rangeMaxY: float
    rangeMaxZ: float
    range: float

@dataclass(slots=True)
class PCRHeader:
    timestamp: int
    sensor_spec: SensorSpec
    session_count: int
    total_frame_count: int
    uncompressed_size: int
    compressed_size: int

@dataclass(slots=True)
class PCRPoint:
    x: float
    y: float
    z: float
    doppler: float
    power: float

@dataclass(slots=True)
class PCRFrameInfo:
    session_id: int
    frame_index: int
    tag: str
    delta_us: int
    accumulated_us: int
    point_count: int
    target_count: int
    points_offset: int
    targets_offset: int

@dataclass(slots=True)
class PCRSessionInfo:
    session_id: int
    frame_count: int
    start_frame_index: int
    end_frame_index: int

# ============================================================================
# Helper Functions
# ============================================================================
def _read_u64(buffer: memoryview, offset: int) -> Tuple[int, int]:
    return struct.unpack_from("<Q", buffer, offset)[0], offset + 8

def _read_u32(buffer: memoryview, offset: int) -> Tuple[int, int]:
    return struct.unpack_from("<I", buffer, offset)[0], offset + 4

def _read_f32(buffer: memoryview, offset: int) -> Tuple[float, int]:
    return struct.unpack_from("<f", buffer, offset)[0], offset + 4

def _read_bytes(buffer: memoryview, offset: int, size: int) -> Tuple[bytes, int]:
    return bytes(buffer[offset:offset + size]), offset + size

def _read_string(buffer: memoryview, offset: int) -> Tuple[str, int]:
    length, offset = _read_u64(buffer, offset)
    data, offset = _read_bytes(buffer, offset, length)
    return data.decode("utf-8", errors="replace"), offset

def _parse_sensor_spec(buffer: memoryview, offset: int) -> Tuple[SensorSpec, int]:
    values: List[float] = []
    for _ in range(16):
        value, offset = _read_f32(buffer, offset)
        values.append(value)
    return SensorSpec(*values), offset

def find_dll_path() -> Path:
    script_dir = Path(__file__).resolve().parent.parent
    candidates = [
        script_dir / "bin" / "x64" / "lzav_lib.dll",
        script_dir / "bin" / "x64" / "lzav_lib.so",
        script_dir / "bin" / "arm64" / "lzav_lib.so",
        script_dir / "bin" / "x64" / "lzav_lib.dylib",
        script_dir / "bin" / "arm64" / "lzav_lib.dylib",
        script_dir / "lzav_lib.dll",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("lzav_lib not found in the expected folders.")

# ============================================================================
# Core Classes
# ============================================================================
class LzavWrapper:
    def __init__(self, dll_path: Path):
        # The wrapper exports a plain C ABI function, so use CDLL on every platform.
        self.dll = ctypes.CDLL(str(dll_path))
        self.dll.LZAV_DecompressBuffer.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        self.dll.LZAV_DecompressBuffer.restype = ctypes.c_int

    def decompress(self, compressed_data: bytes, expected_uncompressed_size: int) -> bytes:
        compressed_len = len(compressed_data)
        src_array = (ctypes.c_uint8 * compressed_len).from_buffer_copy(compressed_data)
        dst_array = (ctypes.c_uint8 * expected_uncompressed_size)()

        result = self.dll.LZAV_DecompressBuffer(
            src_array,
            compressed_len,
            dst_array,
            expected_uncompressed_size,
        )

        if result < 0:
            raise RuntimeError(f"LZAV Decompression failed with error code: {result}")

        return bytes(dst_array[:result])

class PCRFile:
    def __init__(self, pcr_path: Path | str, dll_path: Optional[Path | str] = None):
        self.path = Path(pcr_path)
        self.lzav = LzavWrapper(Path(dll_path) if dll_path else find_dll_path())
        
        self.header: PCRHeader
        self.sessions: List[PCRSessionInfo] = []
        self.frame_infos: List[PCRFrameInfo] = []
        
        self._raw_payload: bytes = b""
        self._load()

    def _parse_header(self, raw: memoryview) -> Tuple[PCRHeader, int]:
        offset = 0
        magic, offset = _read_bytes(raw, offset, 4)
        version, offset = _read_bytes(raw, offset, 4)
        if magic != PCR_MAGIC:
            raise ValueError(f"Invalid PCR magic: {magic!r}")
        if version != PCR_VERSION:
            raise ValueError(f"Unsupported PCR version: {version!r}")

        timestamp, offset = _read_u64(raw, offset)
        sensor_spec, offset = _parse_sensor_spec(raw, offset)
        session_count, offset = _read_u64(raw, offset)
        total_frame_count, offset = _read_u64(raw, offset)
        _, offset = _read_bytes(raw, offset, 32) # Reserved 영역
        uncompressed_size, offset = _read_u64(raw, offset)
        compressed_size, offset = _read_u64(raw, offset)

        header = PCRHeader(
            timestamp=timestamp,
            sensor_spec=sensor_spec,
            session_count=session_count,
            total_frame_count=total_frame_count,
            uncompressed_size=uncompressed_size,
            compressed_size=compressed_size,
        )
        return header, offset

    def _load(self) -> None:
        data = self.path.read_bytes()
        view = memoryview(data)
        
        self.header, payload_offset = self._parse_header(view)
        compressed_payload = data[payload_offset : payload_offset + self.header.compressed_size]

        self._raw_payload = self.lzav.decompress(compressed_payload, self.header.uncompressed_size)
        payload_view = memoryview(self._raw_payload)
        
        offset = 0
        integrity_magic, offset = _read_u64(payload_view, offset)
        if integrity_magic != INTEGRITY_MAGIC:
            raise ValueError(f"Invalid integrity magic in PCR payload: {self.path}")

        global_frame_idx = 0
        self.sessions = []
        self.frame_infos = []

        for _ in range(self.header.session_count):
            session_id, offset = _read_u32(payload_view, offset)
            _, offset = _read_u64(payload_view, offset)  # timestamp
            _, offset = _read_u64(payload_view, offset)  # lengthUs
            _, offset = _read_u64(payload_view, offset)  # bytes
            _, offset = _read_string(payload_view, offset)  # name
            _, offset = _read_string(payload_view, offset)  # description
            tag, offset = _read_string(payload_view, offset)
            frame_count, offset = _read_u64(payload_view, offset)
            
            start_frame_idx = global_frame_idx
            accumulated_us = 0
            
            for _ in range(frame_count):
                packet_size, offset = _read_u32(payload_view, offset)
                frame_count_value, offset = _read_u32(payload_view, offset)
                delta_us, offset = _read_u64(payload_view, offset)
                point_count, offset = _read_u64(payload_view, offset)
                
                points_offset = offset
                offset += point_count * 24  # Point: 5 floats + targetId = 24 bytes
                
                target_count, offset = _read_u64(payload_view, offset)
                targets_offset = offset
                offset += target_count * 40 # Target: 2 floats + 2 uint32 + 6 floats = 40 bytes

                _, offset = _read_u64(payload_view, offset)  # bytes
                
                frame_info = PCRFrameInfo(
                    session_id=session_id,
                    frame_index=global_frame_idx,
                    tag=tag,
                    delta_us=delta_us,
                    accumulated_us=accumulated_us + delta_us,
                    point_count=point_count,
                    target_count=target_count,
                    points_offset=points_offset,
                    targets_offset=targets_offset
                )
                self.frame_infos.append(frame_info)
                accumulated_us += delta_us
                global_frame_idx += 1

            bookmark_count, offset = _read_u64(payload_view, offset)
            for _ in range(bookmark_count):
                _, offset = _read_string(payload_view, offset)
                _, offset = _read_u64(payload_view, offset)
                
            self.sessions.append(PCRSessionInfo(
                session_id=session_id,
                frame_count=frame_count,
                start_frame_index=start_frame_idx,
                end_frame_index=global_frame_idx - 1
            ))

    def get_frame_points(self, frame_index: int) -> List[PCRPoint]:
        info = self.frame_infos[frame_index]
        if info.point_count == 0:
            return []
            
        payload_view = memoryview(self._raw_payload)
        offset = info.points_offset
        
        points = []
        for _ in range(info.point_count):
            x, offset = _read_f32(payload_view, offset)
            y, offset = _read_f32(payload_view, offset)
            z, offset = _read_f32(payload_view, offset)
            doppler, offset = _read_f32(payload_view, offset)
            power, offset = _read_f32(payload_view, offset)
            _, offset = _read_u32(payload_view, offset)  # targetId
            points.append(PCRPoint(x, y, z, doppler, power))
            
        return points

    def get_session_frames(self, session_index: int) -> List[List[PCRPoint]]:
        session = self.sessions[session_index]
        frames = []
        for frame_idx in range(session.start_frame_index, session.end_frame_index + 1):
            frames.append(self.get_frame_points(frame_idx))
        return frames

    def get_frame_raw_targets(self, frame_index: int) -> bytes:
        info = self.frame_infos[frame_index]
        if info.target_count == 0:
            return b""
        start = info.targets_offset
        end = start + (info.target_count * 40)
        return self._raw_payload[start:end]