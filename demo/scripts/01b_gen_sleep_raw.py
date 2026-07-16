#!/usr/bin/env python3
"""demo/sleep.md 검수 완료 후 실행하는 수면 원시 데이터(sleep_session/sleep_stat) 생성 스크립트.

01_gen_raw_data.py 로 이미 만들어진 bin/data/demo.db 에 이어서 실행한다(스키마/다른 테이블은
그대로 두고 sleep_session/sleep_stat 만 채움). 박헬스(user_id=2)는 침실을 쓰지 않으므로 대상에서
제외한다.

실행:
    python3 demo/scripts/01_gen_raw_data.py   # 먼저 실행되어 있어야 함
    python3 demo/scripts/01b_gen_sleep_raw.py
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

from _lib import devices, sleep_model

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "bin" / "data" / "demo.db"

USER_ID = 1
ROOM_ID = devices.ROOM_NAME_TO_ID["침실"]


def main() -> None:
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()

    cur.execute("SELECT COUNT(*) FROM sleep_session")
    if cur.fetchone()[0] > 0:
        print("[01b] sleep_session 이 이미 채워져 있어 초기화 후 다시 생성합니다.")
        cur.execute("DELETE FROM sleep_stat")
        cur.execute("DELETE FROM sleep_session")

    device_list = devices.load_devices()
    _rows, hex_to_pk = devices.build_device_rows(device_list)
    radar_id = hex_to_pk[devices.BED_RADAR_HEX_ID]
    station_id = hex_to_pk[devices.WAVE_STATION_HEX_ID]

    nights = sleep_model.generate_all_nights(USER_ID, ROOM_ID, radar_id, station_id)

    session_count = 0
    stat_count = 0
    for night in nights:
        s = night["session"]
        cur.execute(
            "INSERT INTO sleep_session (user_id, room_id, radar_id, station_id, night_date, onset, "
            "final_wake, time_in_bed_s, asleep_total_s, efficiency, stage_totals, toss_events, hr_mean, "
            "br_mean, snore_ratio) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                s["user_id"], s["room_id"], s["radar_id"], s["station_id"], s["night_date"], s["onset"],
                s["final_wake"], s["time_in_bed_s"], s["asleep_total_s"], s["efficiency"], s["stage_totals"],
                s["toss_events"], s["hr_mean"], s["br_mean"], s["snore_ratio"],
            ),
        )
        session_id = cur.lastrowid
        session_count += 1

        for gran_rows in (night["stat_1m"], night["stat_30m"]):
            for r in gran_rows:
                cur.execute(
                    "INSERT INTO sleep_stat (user_id, room_id, session_id, granularity, time_start, time_end, "
                    "coverage, stage_label, stage_ratio, stage_confidence, status_ratio, toss_mean, toss_max, "
                    "toss_p90, toss_events, toss_ratio, hr_mean, hr_min, hr_max, hr_std, hr_confidence, br_mean, "
                    "br_min, br_max, br_std, snore_ratio, env_temp, env_lux, env_noise, summary_text) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        r["user_id"], r["room_id"], session_id, r["granularity"], r["time_start"], r["time_end"],
                        r["coverage"], r["stage_label"], r["stage_ratio"], r["stage_confidence"], r["status_ratio"],
                        r["toss_mean"], r["toss_max"], r["toss_p90"], r["toss_events"], r["toss_ratio"],
                        r["hr_mean"], r["hr_min"], r["hr_max"], r["hr_std"], r["hr_confidence"], r["br_mean"],
                        r["br_min"], r["br_max"], r["br_std"], r["snore_ratio"], r["env_temp"], r["env_lux"],
                        r["env_noise"], r["summary_text"],
                    ),
                )
                stat_count += 1

    conn.commit()

    print("=== 01b_gen_sleep_raw 완료 ===")
    print(f"sleep_session: {session_count}")
    print(f"sleep_stat: {stat_count}")
    row = cur.execute(
        "SELECT night_date, round(efficiency,3), toss_events, hr_mean FROM sleep_session ORDER BY night_date"
    ).fetchall()
    for r in row[:3] + row[-3:]:
        print(" ", r)

    conn.close()


if __name__ == "__main__":
    main()
