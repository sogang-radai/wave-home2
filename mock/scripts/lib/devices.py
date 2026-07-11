"""장치 목록(bin/device/device_list.json) 로더 + 방 추론 + 두 사용자/방 참조 데이터."""

from __future__ import annotations

import json
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEVICE_LIST_PATH = REPO_ROOT / "bin" / "device" / "device_list.json"


def hex_id(n: int) -> str:
    """DB INTEGER PK ↔ device_list.json 16자리 hex (순차 증가)."""
    return f"{n:016x}"


# 사용자: 김건강(1, 침실 취침) / 박헬스(2, 거실·부엌만 사용, 침실 미사용)
USERS = [
    (1, "김건강", "2025-11-03 10:00:00"),
    (2, "박헬스", "2026-01-12 19:30:00"),
]

# 방: 거실/침실/부엌 — DB id 순서와 동일
ROOMS = [
    (1, "거실", "공용 거실. 카메라/에어컨/선풍기/조명, 두 사람 모두 사용."),
    (2, "침실", "김건강 침실. 레이더 2대(하방/책상)/Wave Station/조명/TV/PC 플러그. 박헬스는 사용하지 않음."),
    (3, "부엌", "공용 부엌. 인덕션 플러그/조명, 두 사람 모두 사용."),
]
ROOM_NAME_TO_ID = {name: rid for rid, name, _ in ROOMS}

# 방-사용자 매핑: 박헬스는 이 집에서 잠을 자지 않는다는 설정이라 침실 제외.
ROOM_USER_MAP = [
    (1, 1),  # 거실 - 김건강
    (2, 1),  # 침실 - 김건강
    (3, 1),  # 부엌 - 김건강
    (1, 2),  # 거실 - 박헬스
    (3, 2),  # 부엌 - 박헬스
]

GESTURE_SETS_JSON = REPO_ROOT / "bin" / "gestures" / "gesture_sets.json"

DESK_SET_WIRE_ID = hex_id(1)
BED_SET_WIRE_ID = hex_id(2)

DESK_RADAR_HEX_ID = hex_id(2)
BED_RADAR_HEX_ID = hex_id(1)
WAVE_STATION_HEX_ID = hex_id(3)
DROID_CAM_HEX_ID = hex_id(4)
LIVING_CAM_HEX_ID = hex_id(5)
FAN_PLUG_HEX_ID = hex_id(6)
PC_PLUG_HEX_ID = hex_id(7)
AIRCON_PLUG_HEX_ID = hex_id(8)
INDUCTION_PLUG_HEX_ID = hex_id(9)
TV_HEX_ID = hex_id(10)
BEDROOM_LIGHT_HEX_ID = hex_id(11)
LIVING_LIGHT_HEX_ID = hex_id(12)
KITCHEN_LIGHT_HEX_ID = hex_id(13)


def load_gesture_sets() -> list[tuple[int, str, int]]:
    """gesture_set 테이블 행: (id, name, archived)."""
    data = json.loads(GESTURE_SETS_JSON.read_text(encoding="utf-8"))
    rows: list[tuple[int, str, int]] = []
    for i, item in enumerate(data["gesture_sets"], start=1):
        archived = 0 if item.get("enabled", True) else 1
        rows.append((i, item["name"], archived))
    return rows


def desk_set_pk() -> int:
    """gesture_set 테이블 PK for Desk Set (wire id 0000000000000001 → usually 1)."""
    data = json.loads(GESTURE_SETS_JSON.read_text(encoding="utf-8"))
    for i, item in enumerate(data["gesture_sets"], start=1):
        if item.get("id") == DESK_SET_WIRE_ID:
            return i
    return 1

PLUG_HEX_TO_APPLIANCE = {
    FAN_PLUG_HEX_ID: "fan",
    PC_PLUG_HEX_ID: "pc",
    AIRCON_PLUG_HEX_ID: "aircon",
    INDUCTION_PLUG_HEX_ID: "induction",
}


def load_devices() -> list[dict]:
    data = json.loads(DEVICE_LIST_PATH.read_text(encoding="utf-8"))
    return data["device_list"]


def infer_room_name(device: dict) -> str:
    """장치 설명/이름에서 방을 추론한다(거실/침실/부엌). 책상 레이더는 침실 고정."""
    if device.get("id") == DESK_RADAR_HEX_ID:
        return "침실"
    if device.get("id") == DROID_CAM_HEX_ID:
        # 설명에 방 이름이 없음(거의 사용되지 않는 보조 카메라) - 거실에 둔 것으로 간주.
        return "거실"
    text = f"{device.get('name', '')} {device.get('description', '')}"
    for room_name in ("침실", "거실", "부엌"):
        if room_name in text:
            return room_name
    raise ValueError(f"방을 추론할 수 없는 장치: {device.get('name')} / {device.get('description')}")


def classify_appliance(description: str) -> str | None:
    """전력 계측 대상 가전 분류(에어컨/컴퓨터/선풍기)."""
    if "에어컨" in description:
        return "aircon"
    if "컴퓨터" in description or "PC" in description:
        return "pc"
    if "선풍기" in description:
        return "fan"
    if "인덕션" in description:
        return "induction"
    return None


def build_device_rows(devices: list[dict]) -> tuple[list[tuple[int, str, str, str, int, int, str]], dict[str, int]]:
    """device 테이블 행과 hex_id -> integer PK 매핑 (JSON id hex = DB PK)."""
    rows: list[tuple[int, str, str, str, int, int, str]] = []
    hex_to_pk: dict[str, int] = {}
    for i, dev in enumerate(devices, start=1):
        pk = i
        rows.append((
            pk,
            dev["name"],
            dev["description"],
            dev["class"],
            0,
            1 if dev.get("enabled", True) else 0,
            "{}",
        ))
        hex_to_pk[dev["id"]] = pk
    return rows, hex_to_pk


def devices_shared_rooms(devices: list[dict]) -> dict[str, set[str]]:
    """장치가 속한 방에 따라 공유 사용자를 정하기 위해, 방 이름만 반환한다."""
    return {dev["id"]: {infer_room_name(dev)} for dev in devices}
