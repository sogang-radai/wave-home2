#!/usr/bin/env python3
"""AI(리포트/인사이트/임베딩)가 필요 없는 원시 데이터를 전부 채운다.

- 계정/방/장치/설정/제스처/홈자동화/일정/알람/알림/채팅 (lib/narrative.py)
- 전력(power_energy) 물리 시뮬레이션 (lib/power_model.py)
- 수면(sleep_session/sleep_stat)은 sleep.md 검수 후 별도 스크립트에서 채운다(현재는 스키마만 생성).

실행:
    python3 mock/scripts/01_gen_raw_data.py
    uv run --with sqlite-vec mock/scripts/01_gen_raw_data.py   # vec_* 테이블까지 생성(비어 있음)
"""

from __future__ import annotations

import random
import sqlite3
from pathlib import Path

from lib import devices, narrative, power_model, schema, timeutil

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "mock" / "data" / "mock.db"
SEED = 20260601
CREATED_AT = "2026-05-20 09:00:00"


def main() -> None:
    if DB_PATH.exists():
        DB_PATH.unlink()
    conn = sqlite3.connect(DB_PATH)
    vec_ready = schema.build_schema(conn, with_vec=True)
    cur = conn.cursor()

    # --- 계정 / 방 ---------------------------------------------------------
    cur.executemany("INSERT INTO user (id, name, created_at) VALUES (?, ?, ?)", devices.USERS)
    cur.execute(
        "INSERT INTO user_session (id, active_user_id, access_token_hash, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?)",
        (1, 1, "wavehome-dev-token", CREATED_AT, CREATED_AT),
    )
    cur.executemany("INSERT INTO room (id, name, description) VALUES (?, ?, ?)", devices.ROOMS)
    cur.executemany("INSERT INTO room_user_map (room_id, user_id) VALUES (?, ?)", devices.ROOM_USER_MAP)

    # --- 장치 ---------------------------------------------------------------
    device_list = devices.load_devices()
    device_rows, hex_to_pk = devices.build_device_rows(device_list)
    cur.executemany(
        "INSERT INTO device (id, name, description, class, archived, enabled, interface_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        device_rows,
    )

    room_users: dict[int, list[int]] = {}
    for room_id, user_id in devices.ROOM_USER_MAP:
        room_users.setdefault(room_id, []).append(user_id)

    device_room_rows: list[tuple[int, int]] = []
    device_user_rows: set[tuple[int, int]] = set()
    for dev in device_list:
        pk = hex_to_pk[dev["id"]]
        room_name = devices.infer_room_name(dev)
        room_id = devices.ROOM_NAME_TO_ID[room_name]
        device_room_rows.append((pk, room_id))
        for uid in room_users.get(room_id, []):
            device_user_rows.add((pk, uid))

    cur.executemany("INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)", device_room_rows)
    cur.executemany("INSERT INTO device_user_map (device_id, user_id) VALUES (?, ?)", sorted(device_user_rows))

    # 이후 서사 생성기에서 이름으로 참조할 수 있게 pk 딕셔너리 구성.
    pks = {
        "desk_radar": hex_to_pk[devices.DESK_RADAR_HEX_ID],
        "bed_radar": hex_to_pk[devices.BED_RADAR_HEX_ID],
        "wave_station": hex_to_pk[devices.WAVE_STATION_HEX_ID],
        "tv": hex_to_pk[devices.TV_HEX_ID],
        "bedroom_light": hex_to_pk[devices.BEDROOM_LIGHT_HEX_ID],
        "living_light": hex_to_pk[devices.LIVING_LIGHT_HEX_ID],
        "kitchen_light": hex_to_pk[devices.KITCHEN_LIGHT_HEX_ID],
        "fan_plug": hex_to_pk[devices.FAN_PLUG_HEX_ID],
        "pc_plug": hex_to_pk[devices.PC_PLUG_HEX_ID],
        "aircon_plug": hex_to_pk[devices.AIRCON_PLUG_HEX_ID],
        "induction_plug": hex_to_pk[devices.INDUCTION_PLUG_HEX_ID],
        "microwave_plug": hex_to_pk[devices.MICROWAVE_PLUG_HEX_ID],
    }

    # --- 사용자 설정 ---------------------------------------------------------
    sleep_config_1 = (
        '{"bedtime":"23:00","wakeTime":"07:00","wakeUpSound":"morning-breeze","acAuto":true,'
        '"acTemp":24,"lightAuto":true,"dimStartMinutes":30,"finalBrightness":10,'
        '"wakeLightRamp":true,"wakeMusic":true,"wakeTvOrAlarm":false}'
    )
    sleep_config_2 = (
        '{"bedtime":"23:30","wakeTime":"06:30","wakeUpSound":"energetic-beat","acAuto":false,'
        '"acTemp":25,"lightAuto":false,"dimStartMinutes":15,"finalBrightness":20,'
        '"wakeLightRamp":false,"wakeMusic":true,"wakeTvOrAlarm":true}'
    )
    cur.executemany(
        "INSERT INTO user_sleep_config (user_id, config, updated_at) VALUES (?, ?, ?)",
        [(1, sleep_config_1, CREATED_AT), (2, sleep_config_2, CREATED_AT)],
    )

    general_1 = '{"theme":"dark","language":"ko","notificationSound":"soft-chime","ttsSpeakerId":"female-warm"}'
    general_2 = '{"theme":"light","language":"ko","notificationSound":"classic-bell","ttsSpeakerId":"male-bright"}'
    cur.executemany(
        "INSERT INTO user_general_settings (user_id, settings, updated_at) VALUES (?, ?, ?)",
        [(1, general_1, CREATED_AT), (2, general_2, CREATED_AT)],
    )

    cur.executemany(
        "INSERT INTO user_ai_agent_settings "
        "(user_id, personal_prompt, selected_model_id, ctrl_enter_send, wave_ai_sound, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        [
            (1, "차분하고 다정한 말투로 답해줘.", "gemma4:12b-mlx", 0, 1, CREATED_AT),
            (2, "핵심만 간결하게 답해줘.", "gemma4:12b-mlx", 0, 1, CREATED_AT),
        ],
    )

    # --- 제스처 --------------------------------------------------------------
    # gesture_sets.json 순서: 1=Desk Set, 2=Bed Set.
    # 침실 책상 레이더(device id=2)는 Desk Set에 고정 매핑한다.
    cur.executemany("INSERT INTO gesture_set (id, name, archived) VALUES (?, ?, ?)", devices.load_gesture_sets())
    desk_set_id = devices.desk_set_pk()
    if desk_set_id != 1:
        raise RuntimeError(f"Desk Set PK expected 1, got {desk_set_id}")
    if pks["desk_radar"] != 2:
        raise RuntimeError(f"desk radar PK expected 2, got {pks['desk_radar']}")
    cur.execute(
        "INSERT INTO gesture_device_map (device_id, gesture_set_id) VALUES (?, ?)",
        (pks["desk_radar"], desk_set_id),
    )

    rng = random.Random(SEED)
    gesture_rows = narrative.gen_gesture_log(pks["desk_radar"], pks["tv"], pks["bedroom_light"], rng)
    cur.executemany(
        "INSERT INTO gesture_log "
        "(gesture_set_id, class_id, timestamp, gesture_name, radar_id, device_id, action, confidence) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        gesture_rows,
    )

    # --- 홈 자동화 -------------------------------------------------------------
    hex_ids = {
        "bed_radar": devices.BED_RADAR_HEX_ID,
        "desk_radar": devices.DESK_RADAR_HEX_ID,
        "tv": devices.TV_HEX_ID,
        "bedroom_light": devices.BEDROOM_LIGHT_HEX_ID,
        "aircon_plug": devices.AIRCON_PLUG_HEX_ID,
        "induction_plug": devices.INDUCTION_PLUG_HEX_ID,
    }
    rule_rows = narrative.gen_automation_rules(hex_ids, CREATED_AT)
    cur.executemany(
        "INSERT INTO automation_rule "
        "(user_id, external_id, name, enabled, cooldown_ms, trigger_json, schedule_json, actions_json, "
        "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        rule_rows,
    )

    home_event_rows = narrative.gen_home_events(pks, gesture_rows, random.Random(SEED + 1))
    cur.executemany(
        "INSERT INTO home_event "
        "(user_id, type, occurred_at, device_id, device_name, message, triggered_by, detail_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        home_event_rows,
    )

    # --- 일정 / 알람 / 알림 / 채팅 ---------------------------------------------
    cur.executemany(
        "INSERT INTO schedule_task "
        "(user_id, title, created_at, created_by, category, schedule_kind, day_of_week, event_date, "
        "start_minute, end_minute, done, source_insight_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        narrative.gen_schedule_tasks(CREATED_AT),
    )

    cur.executemany(
        "INSERT INTO alarm "
        "(user_id, name, time_minute, days_of_week, smart_wake, radar_device_id, device_id, method, "
        "enabled, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        narrative.gen_alarms(pks, CREATED_AT),
    )

    cur.executemany(
        "INSERT INTO notification (user_id, type, message, read, created_at) VALUES (?, ?, ?, ?, ?)",
        narrative.gen_notifications(random.Random(SEED + 2)),
    )

    cur.executemany(
        "INSERT INTO chat_history (id, user_id, title, created_at, updated_at, message) VALUES (?, ?, ?, ?, ?, ?)",
        narrative.gen_chat_histories(random.Random(SEED + 3)),
    )

    conn.commit()

    # --- 전력(power_energy) ---------------------------------------------------
    plug_pk_by_appliance = {
        appliance: hex_to_pk[hex_id] for hex_id, appliance in devices.PLUG_HEX_TO_APPLIANCE.items()
    }
    power_counts = power_model.generate_power_energy(conn, plug_pk_by_appliance)

    conn.commit()

    def count(table: str) -> int:
        return cur.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]

    print("=== 01_gen_raw_data 완료 ===")
    print(f"db: {DB_PATH}")
    vec_count = cur.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name LIKE 'vec_%'"
    ).fetchone()[0]
    print(f"schema_version: 1")
    print(f"vec_* 테이블: {vec_count}/{len(schema.VEC_TABLES)} (ready={vec_ready})")
    if not vec_ready:
        print("  → vec 없음: uv run --with sqlite-vec mock/scripts/01_gen_raw_data.py 로 재생성하거나")
        print("    uv run --with sqlite-vec mock/scripts/00_ensure_schema.py 로 기존 DB에 추가")
    for t in (
        "user", "room", "room_user_map", "device", "device_user_map", "device_room_map",
        "user_sleep_config", "user_general_settings", "user_ai_agent_settings",
        "gesture_set", "gesture_log", "automation_rule", "home_event",
        "schedule_task", "alarm", "notification", "chat_history",
    ):
        print(f"  {t}: {count(t)}")
    print(f"  power_energy: {sum(power_counts.values())} ({power_counts})")

    conn.close()


if __name__ == "__main__":
    main()
