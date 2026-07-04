#!/usr/bin/env python3
"""WaveHome 목업 DB 생성기.

docs/db-schema.md 의 스키마로 SQLite DB 를 만들고 참조/시계열 데이터를 채운다.

- 확정 참조 데이터: 계정 / 방 / 장치 / 장치-방/사용자 맵 / 제스처 세트.
- 전력(power_energy): 스마트 플러그 4개의 직전 30일 5m/1h/24h 에너지 + 플러그 합산(device_id=NULL).

수면/제스처 로그/알림/채팅/인사이트 등 나머지 파생 데이터는 이후 단계에서 추가한다.

사용:
    python scripts/gen_mock_data.py                       # 벡터 테이블 제외하고 생성
    uv run --with sqlite-vec scripts/gen_mock_data.py     # 벡터 테이블 포함(권장)
    python scripts/gen_mock_data.py --out /tmp/x.db
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sqlite3
from collections import Counter, defaultdict
from datetime import date, datetime, timedelta
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEVICE_LIST = REPO_ROOT / "bin" / "data" / "device_list.json"
DEFAULT_OUTPUT = REPO_ROOT / "bin" / "data" / "mock.db"

# 재현성을 위한 고정 시드.
SEED = 20260703

# 목업 기준 날짜(생성일). 전력은 이 날 직전 30일치를 만든다.
ANCHOR_DATE = date(2026, 7, 3)
POWER_DAYS = 30
ANCHOR_CREATED_AT = "2026-06-03 09:00:00"

# ---------------------------------------------------------------------------
# 스키마 (docs/db-schema.md 기준). sqlite-vec 확장이 필요한 vec_* 가상 테이블은
# 여기서 만들지 않는다(에이전트 쪽에서 확장 로드 후 생성).
# ---------------------------------------------------------------------------
SCHEMA_SQL = """
CREATE TABLE user (
    id         INTEGER     NOT NULL,
    name       TEXT        NOT NULL,
    created_at VARCHAR(50),
    PRIMARY KEY (id)
);

CREATE TABLE room (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL,
    PRIMARY KEY (id)
);

CREATE TABLE room_user_map (
    room_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    PRIMARY KEY (room_id, user_id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE device (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL,
    class       TEXT    NOT NULL,
    archived    INTEGER NOT NULL,
    PRIMARY KEY (id)
);

CREATE TABLE device_user_map (
    device_id INTEGER NOT NULL,
    user_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, user_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE device_room_map (
    device_id INTEGER NOT NULL,
    room_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, room_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (room_id) REFERENCES room(id)
);

CREATE TABLE sleep_session (
    id             INTEGER     PRIMARY KEY,
    user_id        INTEGER     NOT NULL,
    room_id        INTEGER     NOT NULL,
    radar_id       INTEGER     NOT NULL,
    station_id     INTEGER,
    night_date     VARCHAR(50) NOT NULL,
    onset          VARCHAR(50),
    final_wake     VARCHAR(50),
    time_in_bed_s  INTEGER,
    asleep_total_s INTEGER,
    efficiency     REAL,
    stage_totals   TEXT,
    toss_events    INTEGER,
    hr_mean        REAL,
    br_mean        REAL,
    snore_ratio    REAL,
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (radar_id) REFERENCES device(id),
    FOREIGN KEY (station_id) REFERENCES device(id)
);

CREATE TABLE sleep_stat (
    id               INTEGER     PRIMARY KEY,
    user_id          INTEGER     NOT NULL,
    room_id          INTEGER     NOT NULL,
    session_id       INTEGER,
    granularity      VARCHAR(3)  NOT NULL,
    time_start       VARCHAR(50) NOT NULL,
    time_end         VARCHAR(50),
    coverage         REAL        NOT NULL,
    stage_label      VARCHAR(10),
    stage_ratio      TEXT,
    stage_confidence REAL,
    status_ratio     TEXT,
    toss_mean        REAL,
    toss_max         REAL,
    toss_p90         REAL,
    toss_events      INTEGER,
    toss_ratio       TEXT,
    hr_mean          REAL,
    hr_min           REAL,
    hr_max           REAL,
    hr_std           REAL,
    hr_confidence    REAL,
    br_mean          REAL,
    br_min           REAL,
    br_max           REAL,
    br_std           REAL,
    snore_ratio      REAL,
    env_temp         REAL,
    env_lux          REAL,
    env_noise        REAL,
    summary_text     TEXT,
    CHECK (granularity IN ('1m', '30m')),
    UNIQUE (user_id, granularity, time_start),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (session_id) REFERENCES sleep_session(id)
);

CREATE TABLE sleep_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period       VARCHAR(10) NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    session_id   INTEGER,
    metrics      TEXT,
    report_text  TEXT,
    CHECK (period IN ('daily', 'weekly')),
    UNIQUE (user_id, period, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (session_id) REFERENCES sleep_session(id)
);

CREATE TABLE power_energy (
    id           INTEGER     PRIMARY KEY,
    device_id    INTEGER,
    granularity  VARCHAR(3)  NOT NULL,
    time_start   VARCHAR(50) NOT NULL,
    energy_wh    REAL        NOT NULL,
    coverage     REAL        NOT NULL,
    sample_count INTEGER     NOT NULL,
    CHECK (granularity IN ('5m', '1h', '24h', '1w', '1mo')),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE UNIQUE INDEX uq_power_energy ON power_energy (COALESCE(device_id, -1), granularity, time_start);

CREATE TABLE power_report (
    id           INTEGER     PRIMARY KEY,
    energy_id    INTEGER     NOT NULL,
    device_id    INTEGER,
    period       VARCHAR(10) NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    metrics      TEXT,
    report_text  TEXT,
    created_at   VARCHAR(50),
    CHECK (period IN ('1h', '24h', '1w', '1mo')),
    FOREIGN KEY (energy_id) REFERENCES power_energy(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE UNIQUE INDEX uq_power_report ON power_report (COALESCE(device_id, -1), period, period_start);

CREATE TABLE gesture_set (
    id       INTEGER NOT NULL,
    name     TEXT    NOT NULL,
    archived INTEGER NOT NULL,
    PRIMARY KEY (id)
);

CREATE TABLE gesture_log (
    id             INTEGER     PRIMARY KEY,
    gesture_set_id INTEGER     NOT NULL,
    class_id       INTEGER     NOT NULL,
    timestamp      VARCHAR(50) NOT NULL,
    gesture_name   VARCHAR(50) NOT NULL,
    radar_id       INTEGER     NOT NULL,
    device_id      INTEGER,
    action         VARCHAR(100),
    confidence     REAL,
    FOREIGN KEY (gesture_set_id) REFERENCES gesture_set(id),
    FOREIGN KEY (radar_id) REFERENCES device(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE INDEX idx_gesture_log_occurred ON gesture_log (timestamp);

CREATE TABLE routine_task (
    id           INTEGER      PRIMARY KEY,
    user_id      INTEGER      NOT NULL,
    title        VARCHAR(100) NOT NULL,
    created_at   VARCHAR(50),
    created_by   VARCHAR(10)  NOT NULL,
    category     VARCHAR(10)  NOT NULL,
    day_of_week  VARCHAR(3)   NOT NULL,
    start_minute INTEGER,
    end_minute   INTEGER,
    done         INTEGER      NOT NULL,
    CHECK (day_of_week IN ('mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun')),
    CHECK (created_by IN ('user', 'agent')),
    CHECK ((start_minute IS NULL AND end_minute IS NULL)
           OR (start_minute >= 0 AND start_minute < end_minute AND end_minute <= 1440)),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_routine_task_user_day ON routine_task (user_id, day_of_week);

CREATE TABLE notification (
    id         INTEGER      PRIMARY KEY,
    user_id    INTEGER      NOT NULL,
    type       VARCHAR(20)  NOT NULL,
    message    VARCHAR(200) NOT NULL,
    read       INTEGER      NOT NULL,
    created_at VARCHAR(50)  NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_notification_user_created ON notification (user_id, created_at);

CREATE TABLE chat_history (
    id         INTEGER      NOT NULL,
    user_id    INTEGER      NOT NULL,
    created_at VARCHAR(50)  NOT NULL,
    message    TEXT         NOT NULL,
    PRIMARY KEY (id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE insight (
    id         INTEGER      PRIMARY KEY,
    user_id    INTEGER      NOT NULL,
    domain     VARCHAR(20)  NOT NULL,
    period     VARCHAR(10),
    title      VARCHAR(100) NOT NULL,
    text       VARCHAR(300) NOT NULL,
    approved   INTEGER      NOT NULL,
    created_at VARCHAR(50)  NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_insight_user_domain ON insight (user_id, domain);
"""

# ---------------------------------------------------------------------------
# 확정 데이터
# ---------------------------------------------------------------------------
USERS = [
    (1, "김건강"),
    (2, "박웰빙"),
]

# id, name, description
ROOMS = [
    (1, "침실", "김건강 침실. 레이더/Wave Station/조명/TV/PC 플러그."),
    (2, "거실", "공용 거실. 카메라/에어컨/선풍기/조명."),
    (3, "부엌", "공용 부엌. 선풍기 플러그/조명."),
]

ROOM_NAME_TO_ID = {name: rid for rid, name, _ in ROOMS}

# 방-사용자 매핑: 김건강은 전 구역, 박웰빙은 침실 제외(김건강 침실이라 가정).
ROOM_USER_MAP = [
    (1, 1),  # 침실 - 김건강
    (2, 1),  # 거실 - 김건강
    (3, 1),  # 부엌 - 김건강
    (2, 2),  # 거실 - 박웰빙
    (3, 2),  # 부엌 - 박웰빙
]

# 제스처 세트: 우선 Desk Set 하나만.
GESTURE_SETS = [
    (1, "Desk Set", 0),
]


def infer_room_id(device: dict) -> int:
    """장치 설명/이름에서 방을 추론한다(침실/거실/부엌)."""
    text = f"{device.get('name', '')} {device.get('description', '')}"
    for room_name in ("침실", "거실", "부엌"):
        if room_name in text:
            return ROOM_NAME_TO_ID[room_name]
    raise ValueError(f"방을 추론할 수 없는 장치: {device.get('name')} / {device.get('description')}")


def load_devices(device_list_path: Path) -> list[dict]:
    data = json.loads(device_list_path.read_text(encoding="utf-8"))
    return data["device_list"]


# ---------------------------------------------------------------------------
# 전력 모델링
#
# 계측 가능한 장치는 tuya_ep2h 플러그뿐(에어컨/거실선풍기/PC/부엌선풍기).
# 1분 1샘플로 와트를 시뮬레이션한 뒤 5m 버킷으로 적분하고, 5m -> 1h -> 24h 로 롤업한다.
# 규칙:
#   - 평일 09~18 출근(부재), 주말 재택, 23~07 취침(거실 비움).
#   - 7월에 가까울수록 냉방 사용 증가(heat_factor).
#   - 대기전력이 있는 가전(에어컨/PC)은 off 여도 대기 와트로 행을 남긴다.
#   - 소비가 0 인 구간(선풍기 off 등)은 행을 생략한다.
#   - device_id=NULL 합산 행은 플러그만 합산한다.
#   - 요금(cost)은 저장하지 않는다(백엔드가 energy 로 추정).
# ---------------------------------------------------------------------------
POWER_PLUG_CLASS = "tuya_ep2h"
RAW_STEP_MIN = 1                       # 원시 샘플 주기(분)
BUCKET_MIN = 5                         # 최소 저장 단위(분)
SAMPLES_PER_BUCKET = BUCKET_MIN // RAW_STEP_MIN
DISCONNECT_PROB = 0.004               # 원시 샘플이 빠질 확률 -> coverage < 1


def _clampf(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def _standby(watt: float, rng: random.Random) -> float:
    return max(0.0, rng.gauss(watt, watt * 0.15))


def classify_appliance(description: str) -> str | None:
    if "에어컨" in description:
        return "aircon"
    if "컴퓨터" in description or "PC" in description:
        return "pc"
    if "선풍기" in description:
        return "fan"
    return None


def heat_factor(day_index: int, rng: random.Random) -> float:
    """초여름(0.30) -> 7월 초(0.90) 로 갈수록 더워진다. 하루 편차 포함."""
    base = 0.30 + 0.60 * (day_index / max(1, POWER_DAYS - 1))
    return _clampf(base + rng.uniform(-0.12, 0.12), 0.0, 1.0)


def occupancy(dt: datetime) -> tuple[bool, bool]:
    """(home, sleeping). 평일 09~18 부재, 주말 재택, 23~07 취침."""
    weekend = dt.weekday() >= 5
    sleeping = dt.hour >= 23 or dt.hour < 7
    home = True if weekend else (dt.hour < 9 or dt.hour >= 18)
    return home, sleeping


def day_flags(appliance: str, heat: float, rng: random.Random) -> dict:
    if appliance == "aircon":
        return {"used": rng.random() < 0.15 + 0.80 * heat}
    if appliance == "fan":
        return {"used": rng.random() < 0.45 + 0.45 * heat}
    if appliance == "pc":
        return {"session": rng.random() < 0.90}
    return {}


def _watt_aircon(dt, heat, flags, home, sleeping, rng) -> float:
    if not flags["used"]:
        return _standby(2.0, rng)
    occupied_living = home and not sleeping
    elig = (10 <= dt.hour < 23) if dt.weekday() >= 5 else (18 <= dt.hour < 23)
    if occupied_living and elig:
        duty = _clampf(0.35 + 0.50 * heat, 0.0, 0.95)
        if rng.random() < duty:
            return _clampf(rng.gauss(880, 45), 650, 1100)  # 컴프레서 가동
        return _clampf(rng.gauss(52, 6), 35, 80)           # 실내기 송풍
    return _standby(2.0, rng)


def _watt_fan(dt, room, flags, home, sleeping, rng) -> float:
    h = dt.hour + dt.minute / 60.0
    weekend = dt.weekday() >= 5
    if room == "부엌":
        if weekend:
            cook = (8.0 <= h < 10.0) or (18.0 <= h < 20.0)
        else:
            cook = (7.0 <= h < 8.0) or (18.0 <= h < 19.5)
        if home and cook and rng.random() < 0.70:
            return _clampf(rng.gauss(40, 4), 25, 60)
        return 0.0
    # 거실 선풍기
    if not flags["used"]:
        return 0.0
    occupied_living = home and not sleeping
    elig = (11 <= dt.hour < 23) if weekend else (18 <= dt.hour < 23)
    if occupied_living and elig and rng.random() < 0.80:
        return _clampf(rng.gauss(45, 5), 30, 65)
    return 0.0


def _watt_pc(dt, flags, home, rng) -> float:
    h = dt.hour + dt.minute / 60.0
    win = (13.0 <= h < 23.5) if dt.weekday() >= 5 else (19.0 <= h < 23.5)
    if flags["session"] and home and win:
        if rng.random() < 0.15:
            return _clampf(rng.gauss(210, 30), 140, 320)  # 로드
        return _clampf(rng.gauss(80, 8), 55, 120)         # 아이들
    return _standby(4.0, rng)  # 대기전력 유지


def _rollup(rows: list[tuple], key_fn) -> list[tuple]:
    """(dev_id, ts, e, cov, sc) 행을 상위 단위로 합산. coverage 는 평균."""
    buckets: dict[tuple, list] = {}
    for dev_id, ts, e, cov, sc in rows:
        b = buckets.setdefault((dev_id, key_fn(ts)), [0.0, 0.0, 0, 0])
        b[0] += e
        b[1] += cov
        b[2] += sc
        b[3] += 1
    return [(dev_id, nts, e, cov_sum / n, sc) for (dev_id, nts), (e, cov_sum, sc, n) in buckets.items()]


def _aggregate(rows: list[tuple]) -> list[tuple]:
    """플러그 합산(device_id=NULL). coverage 는 평균."""
    buckets: dict[str, list] = {}
    for _dev_id, ts, e, cov, sc in rows:
        b = buckets.setdefault(ts, [0.0, 0.0, 0, 0])
        b[0] += e
        b[1] += cov
        b[2] += sc
        b[3] += 1
    return [(None, ts, e, cov_sum / n, sc) for ts, (e, cov_sum, sc, n) in buckets.items()]


APPLIANCE_KR = {"aircon": "에어컨", "fan": "선풍기", "pc": "컴퓨터"}
ACTIVE_W_THRESHOLD = 10.0              # 대기전력과 실사용을 가르는 경계(W)
AVG_W_FROM_5M = 60.0 / BUCKET_MIN     # 5m energy_wh -> 평균 W 환산 계수(=12)
REPORT_CREATED_AT = f"{ANCHOR_DATE} 09:00:00"


def _build_power_reports(
    cur: sqlite3.Cursor,
    rows_5m: list[tuple],
    agg_5m: list[tuple],
    rows_1h: list[tuple],
    agg_1h: list[tuple],
    rows_24h: list[tuple],
    agg_24h: list[tuple],
    dates: list[date],
    plug_label: dict[int, str],
) -> dict[str, int]:
    """1h/24h 리포트 + 슬라이딩 주간(1w)/월간(1mo) 에너지·리포트를 만든다. report_text 는 NULL."""
    all_5m = [(d, t, e) for d, t, e, _c, _s in rows_5m]
    all_5m += [(None, t, e) for _d, t, e, _c, _s in agg_5m]

    # 5m 에서 피크 전력/실사용 구간 집계(1h, 24h 키).
    peak: dict[tuple, tuple[float, str]] = {}   # (dev, gran, ts) -> (peak_w, peak_at)
    active: dict[tuple, int] = {}               # (dev, gran, ts) -> 실사용 5m 개수
    for dev, ts, e in all_5m:
        w = e * AVG_W_FROM_5M
        for gran, key in (("1h", ts[:13] + ":00:00"), ("24h", ts[:10])):
            k = (dev, gran, key)
            if k not in peak or w > peak[k][0]:
                peak[k] = (w, ts)
            if w > ACTIVE_W_THRESHOLD:
                active[k] = active.get(k, 0) + 1

    # 에너지/커버리지/샘플 맵 + 합산 리포트의 장치별 분해.
    energy_of: dict[tuple, float] = {}          # (dev, gran, ts) -> energy_wh
    cov_of: dict[tuple, float] = {}
    sc_of: dict[tuple, int] = {}
    per_device_by: dict[tuple, list] = {}       # (gran, ts) -> [(dev, energy_wh)]
    for gran, dev_rows, agg_rows in (("1h", rows_1h, agg_1h), ("24h", rows_24h, agg_24h)):
        for dev, ts, e, cov, sc in dev_rows:
            energy_of[(dev, gran, ts)] = e
            cov_of[(dev, gran, ts)] = cov
            sc_of[(dev, gran, ts)] = sc
            per_device_by.setdefault((gran, ts), []).append((dev, e))
        for _dev, ts, e, cov, sc in agg_rows:
            energy_of[(None, gran, ts)] = e
            cov_of[(None, gran, ts)] = cov
            sc_of[(None, gran, ts)] = sc

    # ---- 슬라이딩 주간(7일)/월간(30일) 에너지 롤업: 24h 를 창 단위로 합산 ----
    devices_set = list(plug_label.keys()) + [None]
    window_rows: list[tuple] = []               # (dev, gran, ts0, e, cov, sc, days)
    win_days: dict[tuple, int] = {}             # (dev, gran, ts0) -> 데이터 있는 날 수
    for gran, size in (("1w", 7), ("1mo", 30)):
        for i in range(0, len(dates) - size + 1):  # 창이 데이터 범위에 완전히 들어갈 때만
            win = [dates[j].strftime("%Y-%m-%d") for j in range(i, i + size)]
            ts0 = win[0]
            for dev in devices_set:
                present = [d for d in win if (dev, "24h", d) in energy_of]
                if not present:
                    continue
                e = sum(energy_of[(dev, "24h", d)] for d in present)
                cov = sum(cov_of[(dev, "24h", d)] for d in present) / len(present)
                sc = sum(sc_of[(dev, "24h", d)] for d in present)
                window_rows.append((dev, gran, ts0, e, cov, sc, len(present)))
                energy_of[(dev, gran, ts0)] = e
                win_days[(dev, gran, ts0)] = len(present)
                pw, pat, act = 0.0, ts0, 0
                for d in present:
                    dp = peak.get((dev, "24h", d))
                    if dp and dp[0] > pw:
                        pw, pat = dp
                    act += active.get((dev, "24h", d), 0)
                peak[(dev, gran, ts0)] = (pw, pat)
                active[(dev, gran, ts0)] = act
                if dev is None:
                    parts = []
                    for pid in plug_label:
                        pe = sum(energy_of[(pid, "24h", d)] for d in win if (pid, "24h", d) in energy_of)
                        if pe > 0:
                            parts.append((pid, pe))
                    per_device_by[(gran, ts0)] = parts

    if window_rows:
        cur.executemany(
            "INSERT INTO power_energy (device_id, granularity, time_start, energy_wh, coverage, sample_count)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            [(dev, gran, ts, round(e, 4), round(cov, 4), sc) for dev, gran, ts, e, cov, sc, _d in window_rows],
        )

    # 원본 power_energy 행 id 조회: (device_id, granularity, time_start) -> id
    id_map: dict[tuple, int] = {}
    for eid, dev_id, gran, ts in cur.execute(
        "SELECT id, device_id, granularity, time_start FROM power_energy"
        " WHERE granularity IN ('1h','24h','1w','1mo')"
    ):
        id_map[(dev_id, gran, ts)] = eid

    expected = {"1h": 12, "24h": 288, "1w": 7 * 288, "1mo": 30 * 288}

    def _prev_ts(gran: str, ts: str) -> str | None:
        if gran == "1h":
            return (datetime.strptime(ts, "%Y-%m-%d %H:%M:%S") - timedelta(hours=1)).strftime("%Y-%m-%d %H:00:00")
        if gran == "24h":
            return (datetime.strptime(ts, "%Y-%m-%d") - timedelta(days=1)).strftime("%Y-%m-%d")
        if gran == "1w":  # 한 주기(7일) 이전의 비겹침 창
            return (datetime.strptime(ts, "%Y-%m-%d") - timedelta(days=7)).strftime("%Y-%m-%d")
        return None

    # 리포트 대상: 1h/24h 원본 행 + 주간/월간 창
    targets: list[tuple] = []
    for gran, dev_rows, agg_rows in (("1h", rows_1h, agg_1h), ("24h", rows_24h, agg_24h)):
        for dev, ts, e, cov, _sc in list(dev_rows) + list(agg_rows):
            targets.append((dev, gran, ts, e, cov))
    for dev, gran, ts, e, cov, _sc, _d in window_rows:
        targets.append((dev, gran, ts, e, cov))

    report_rows = []
    for dev, gran, ts, e, cov in targets:
        k = (dev, gran, ts)
        pw, pat = peak.get(k, (0.0, ts))
        metrics = {
            "energy_wh": round(e, 2),
            "energy_kwh": round(e / 1000.0, 3),
            "peak_w": round(pw, 1),
            "peak_at": pat,
            "on_ratio": round(active.get(k, 0) / expected[gran], 3),
            "coverage": round(cov, 3),
        }
        if gran in ("1w", "1mo"):
            days = win_days.get(k, 0)
            metrics["days"] = days
            metrics["avg_daily_wh"] = round(e / days, 2) if days else 0.0
        pts = _prev_ts(gran, ts)
        prev = energy_of.get((dev, gran, pts)) if pts else None
        if prev is not None:
            metrics["prev_energy_wh"] = round(prev, 2)
            metrics["vs_prev_pct"] = round((e - prev) / prev * 100.0, 1) if prev > 0 else None
        if dev is None:
            parts = sorted(per_device_by.get((gran, ts), []), key=lambda t: t[1], reverse=True)
            metrics["by_device"] = [
                {
                    "device_id": d,
                    "name": plug_label.get(d, str(d)),
                    "energy_wh": round(pe, 2),
                    "share": round(pe / e, 3) if e > 0 else 0.0,
                }
                for d, pe in parts
            ]
        report_rows.append(
            (id_map[k], dev, gran, ts, json.dumps(metrics, ensure_ascii=False), REPORT_CREATED_AT)
        )

    cur.executemany(
        "INSERT INTO power_report (energy_id, device_id, period, period_start, metrics, report_text, created_at)"
        " VALUES (?, ?, ?, ?, ?, NULL, ?)",
        report_rows,
    )

    return {
        "energy_1w": sum(1 for r in window_rows if r[1] == "1w"),
        "energy_1mo": sum(1 for r in window_rows if r[1] == "1mo"),
        "report": len(report_rows),
    }


def generate_power(conn: sqlite3.Connection, devices: list[dict], rng: random.Random) -> dict[str, int]:
    plugs = []
    plug_label: dict[int, str] = {}
    for dev in devices:
        if dev.get("class") != POWER_PLUG_CLASS:
            continue
        appliance = classify_appliance(dev.get("description", ""))
        if appliance is None:
            continue
        text = f"{dev.get('name', '')} {dev.get('description', '')}"
        room = next((rn for rn in ("침실", "거실", "부엌") if rn in text), None)
        dev_id = int(dev["id"], 16)
        plugs.append({"id": dev_id, "appliance": appliance, "room": room})
        plug_label[dev_id] = f"{room or ''} {APPLIANCE_KR.get(appliance, appliance)}".strip()

    start_date = ANCHOR_DATE - timedelta(days=POWER_DAYS)

    rows_5m: list[tuple] = []
    for plug in plugs:
        for di in range(POWER_DAYS):
            day = start_date + timedelta(days=di)
            heat = heat_factor(di, rng)
            flags = day_flags(plug["appliance"], heat, rng)
            day0 = datetime(day.year, day.month, day.day)

            minute_series = []
            for m in range(24 * 60):
                dt = day0 + timedelta(minutes=m)
                home, sleeping = occupancy(dt)
                if plug["appliance"] == "aircon":
                    w = _watt_aircon(dt, heat, flags, home, sleeping, rng)
                elif plug["appliance"] == "pc":
                    w = _watt_pc(dt, flags, home, rng)
                else:
                    w = _watt_fan(dt, plug["room"], flags, home, sleeping, rng)
                connected = rng.random() > DISCONNECT_PROB
                minute_series.append((connected, w))

            for b in range(24 * 60 // BUCKET_MIN):
                seg = minute_series[b * BUCKET_MIN:(b + 1) * BUCKET_MIN]
                present = [w for (c, w) in seg if c]
                if not present:
                    continue  # 전부 연결 끊김 -> 데이터 없음
                total_w = sum(present)
                if total_w <= 0.0:
                    continue  # off(소비 없음) -> 행 생략
                energy_wh = total_w * (RAW_STEP_MIN / 60.0)
                coverage = len(present) / SAMPLES_PER_BUCKET
                ts = (day0 + timedelta(minutes=b * BUCKET_MIN)).strftime("%Y-%m-%d %H:%M:00")
                rows_5m.append((plug["id"], ts, energy_wh, coverage, len(present)))

    rows_1h = _rollup(rows_5m, lambda ts: ts[:13] + ":00:00")
    rows_24h = _rollup(rows_1h, lambda ts: ts[:10])
    agg_5m = _aggregate(rows_5m)
    agg_1h = _aggregate(rows_1h)
    agg_24h = _aggregate(rows_24h)

    cur = conn.cursor()

    def _insert(rows: list[tuple], gran: str) -> None:
        cur.executemany(
            "INSERT INTO power_energy (device_id, granularity, time_start, energy_wh, coverage, sample_count)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            [(r[0], gran, r[1], round(r[2], 4), round(r[3], 4), r[4]) for r in rows],
        )

    for rows, gran in ((rows_5m, "5m"), (rows_1h, "1h"), (rows_24h, "24h"),
                       (agg_5m, "5m"), (agg_1h, "1h"), (agg_24h, "24h")):
        _insert(rows, gran)

    dates = [start_date + timedelta(days=i) for i in range(POWER_DAYS)]
    rep = _build_power_reports(cur, rows_5m, agg_5m, rows_1h, agg_1h, rows_24h, agg_24h, dates, plug_label)
    conn.commit()

    return {
        "power_energy 5m": len(rows_5m) + len(agg_5m),
        "power_energy 1h": len(rows_1h) + len(agg_1h),
        "power_energy 24h": len(rows_24h) + len(agg_24h),
        "power_energy 1w": rep["energy_1w"],
        "power_energy 1mo": rep["energy_1mo"],
        "power_report": rep["report"],
    }


# ---------------------------------------------------------------------------
# 수면 모델링 (완전 합성, 김건강만)
#
# 레이더(SleepNet) 실측은 status(absent/awake/asleep)와 뒤척임(toss)만 준다.
# 수면단계(light/deep/rem)/HR/BR/코골이/환경은 이 목업에서 합성해 채운다.
# 갭은 없다고 가정(coverage=1.0). 시드는 전력과 분리된 스트림을 쓴다.
# ---------------------------------------------------------------------------
SLEEP_USER_ID = 1
SLEEP_ROOM_ID = 1                          # 침실
SLEEP_STAGES = ("absent", "awake", "light", "deep", "rem")
STAGE_KR = {"awake": "각성", "light": "얕은수면", "deep": "깊은수면", "rem": "렘수면", "absent": "부재"}
HR_BASE = {"awake": 70.0, "light": 60.0, "deep": 54.0, "rem": 64.0}
BR_BASE = {"awake": 16.0, "light": 14.0, "deep": 13.0, "rem": 16.0}
TOSS_BASE = {"awake": (0.50, 0.15), "light": (0.22, 0.08), "deep": (0.09, 0.04), "rem": (0.28, 0.10)}
SNORE_BASE = {"awake": 0.02, "light": 0.12, "deep": 0.35, "rem": 0.25}

_SLEEP_STAT_INSERT = (
    "INSERT INTO sleep_stat (user_id,room_id,session_id,granularity,time_start,time_end,coverage,"
    "stage_label,stage_ratio,stage_confidence,status_ratio,toss_mean,toss_max,toss_p90,toss_events,toss_ratio,"
    "hr_mean,hr_min,hr_max,hr_std,hr_confidence,br_mean,br_min,br_max,br_std,snore_ratio,"
    "env_temp,env_lux,env_noise,summary_text) VALUES (" + ",".join(["?"] * 30) + ")"
)


def _r1(x):
    return round(x, 1) if x is not None else None


def _r2(x):
    return round(x, 2) if x is not None else None


def _r3(x):
    return round(x, 3) if x is not None else None


def _mean(xs):
    return sum(xs) / len(xs) if xs else None


def _pstd(xs):
    if len(xs) < 2:
        return 0.0
    m = sum(xs) / len(xs)
    return (sum((x - m) ** 2 for x in xs) / len(xs)) ** 0.5


def _p90(xs):
    if not xs:
        return 0.0
    s = sorted(xs)
    return s[min(len(s) - 1, int(round(0.9 * (len(s) - 1))))]


def _toss_events(seq) -> int:
    """toss_index 가 0.5 위로 올라오는 상승 교차 수."""
    ev, above = 0, False
    for v in seq:
        if v >= 0.5 and not above:
            ev += 1
            above = True
        elif v < 0.5:
            above = False
    return ev


def _toss_label(idx: float) -> str:
    if idx < 0.34:
        return "calm"
    if idx < 0.66:
        return "slight"
    return "moderate"


def _status_of(stage: str) -> str:
    if stage in ("absent", "awake"):
        return stage
    return "asleep"


def _ratio(counter: Counter, keys) -> dict:
    tot = sum(counter.values())
    if not tot:
        return {}
    return {k: round(counter[k] / tot, 3) for k in keys if counter.get(k, 0)}


def _sleep_night_bounds(weekend: bool, rng: random.Random):
    """자정 기준 분 단위 (bedtime, onset, wake, out_of_bed) 반환."""
    if weekend:
        bedtime = rng.gauss(1455, 45)          # ~00:15(+/-)
        wake = 1440 + rng.gauss(500, 40)       # ~08:20 다음날
    else:
        bedtime = rng.gauss(1380, 30)          # ~23:00
        wake = 1440 + rng.gauss(420, 25)       # ~07:00 다음날
    bedtime = int(_clampf(bedtime, 1330, 1500))
    wake = int(_clampf(wake, 1740, 1980))
    onset = bedtime + rng.randint(6, 30)
    if onset >= wake:
        onset = bedtime + 6
    out = wake + int(_clampf(rng.gauss(12, 5), 3, 30))
    return bedtime, onset, wake, out


def _build_hypnogram(onset: int, wake: int, rng: random.Random) -> list[str]:
    """onset..wake 분 단위 단계 리스트(전반부 deep↑, 후반부 rem↑, 간헐 WASO)."""
    total = wake - onset
    stages: list[str] = []
    c = 0
    while len(stages) < total:
        cyc = int(_clampf(rng.gauss(90, 10), 60, 120))
        deep = int(cyc * max(0.05, 0.30 - 0.06 * c))
        rem = int(cyc * min(0.25, 0.06 + 0.045 * c))
        for stage, dur in (("light", rng.randint(12, 22)), ("deep", deep),
                           ("light", rng.randint(8, 18)), ("rem", rem)):
            stages.extend([stage] * dur)
        if rng.random() < 0.15 + 0.10 * c:
            stages.extend(["awake"] * rng.randint(1, 5))
        c += 1
    return stages[:total]


def _night_minutes(D0: datetime, bedtime: int, onset: int, wake: int, out: int,
                   hypno: list[str], heat: float, rng: random.Random) -> list[dict]:
    total = out - bedtime
    absent = None
    if rng.random() < 0.30:  # 야간 화장실 등 짧은 부재
        alen = rng.randint(3, 8)
        aat = rng.randint(onset + 30, max(onset + 31, wake - 40))
        absent = (aat, aat + alen)

    mins = []
    for m in range(total):
        clock = bedtime + m
        if clock < onset or clock >= wake:
            stage = "awake"
        else:
            stage = hypno[clock - onset] if (clock - onset) < len(hypno) else "light"
        if absent and absent[0] <= clock < absent[1]:
            stage = "absent"
        status = _status_of(stage)
        frac = _clampf((clock - onset) / max(1, wake - onset), 0.0, 1.0)
        night_frac = m / max(1, total)

        if stage == "absent":
            hr = br = hrconf = brconf = None
            toss, snore, conf = 0.0, False, 0.6
        else:
            drift = -3.0 * math.sin(math.pi * frac)
            hr = _clampf(HR_BASE[stage] + drift + rng.gauss(0, 2.5), 45, 95)
            br = _clampf(BR_BASE[stage] + rng.gauss(0, 0.8), 9, 24)
            tm, tsd = TOSS_BASE[stage]
            toss = _clampf(rng.gauss(tm, tsd) + (rng.uniform(0.2, 0.4) if rng.random() < 0.03 else 0.0), 0.0, 1.0)
            snore = status == "asleep" and rng.random() < SNORE_BASE[stage] * (0.6 + 0.8 * heat)
            conf = _clampf(rng.gauss(0.85, 0.05), 0.5, 0.99)
            hrconf = _clampf(rng.gauss(0.80, 0.07), 0.4, 0.98)
            brconf = _clampf(rng.gauss(0.82, 0.06), 0.4, 0.98)

        temp = _clampf(24.0 + 4.0 * heat - 1.5 * night_frac + rng.gauss(0, 0.25), 20, 32)
        if clock >= wake - 30:  # 기상 30분 전 조명 자동화
            lux = _clampf((clock - (wake - 30)) / 30.0 * 40.0, 0, 45)
        else:
            lux = _clampf(0.5 + rng.gauss(0, 0.3), 0, 5)
        noise = _clampf(30.0 + rng.gauss(0, 1.5) + (8.0 if snore else 0.0), 25, 60)

        mins.append({"clock": clock, "stage": stage, "status": status, "hr": hr, "br": br,
                     "toss": toss, "tlabel": _toss_label(toss), "snore": snore, "conf": conf,
                     "hrconf": hrconf, "brconf": brconf, "temp": temp, "lux": lux, "noise": noise})
    return mins


def _stat_row_1m(sid: int, D0: datetime, mm: dict) -> tuple:
    ts = (D0 + timedelta(minutes=mm["clock"])).strftime("%Y-%m-%d %H:%M:00")
    te = (D0 + timedelta(minutes=mm["clock"] + 1)).strftime("%Y-%m-%d %H:%M:00")
    hr, br = mm["hr"], mm["br"]
    return (
        SLEEP_USER_ID, SLEEP_ROOM_ID, sid, "1m", ts, te, 1.0,
        mm["stage"], json.dumps({mm["stage"]: 1.0}, ensure_ascii=False), _r3(mm["conf"]),
        json.dumps({mm["status"]: 1.0}, ensure_ascii=False),
        _r3(mm["toss"]), _r3(mm["toss"]), _r3(mm["toss"]), 1 if mm["toss"] >= 0.5 else 0,
        json.dumps({mm["tlabel"]: 1.0}, ensure_ascii=False),
        _r1(hr), _r1(hr), _r1(hr), 0.0 if hr is not None else None, _r3(mm["hrconf"]),
        _r1(br), _r1(br), _r1(br), 0.0 if br is not None else None,
        1.0 if mm["snore"] else 0.0,
        _r2(mm["temp"]), _r2(mm["lux"]), _r2(mm["noise"]), None,
    )


def _stat_row_30m(sid: int, D0: datetime, win_idx: int, mms: list[dict]) -> tuple:
    ts = (D0 + timedelta(minutes=win_idx)).strftime("%Y-%m-%d %H:%M:00")
    te = (D0 + timedelta(minutes=win_idx + 30)).strftime("%Y-%m-%d %H:%M:00")
    stage_ct = Counter(mm["stage"] for mm in mms)
    status_ct = Counter(mm["status"] for mm in mms)
    tl_ct = Counter(mm["tlabel"] for mm in mms)
    stage_label = stage_ct.most_common(1)[0][0]
    tosses = [mm["toss"] for mm in mms]
    hr = [mm["hr"] for mm in mms if mm["hr"] is not None]
    br = [mm["br"] for mm in mms if mm["br"] is not None]
    asleep = [mm for mm in mms if mm["status"] == "asleep"]
    snore_ct = sum(1 for mm in asleep if mm["snore"])
    conf = [mm["conf"] for mm in mms]
    hrc = [mm["hrconf"] for mm in mms if mm["hrconf"] is not None]
    summary = _sleep_summary(ts, stage_label, hr, br, tosses, snore_ct, len(asleep))
    return (
        SLEEP_USER_ID, SLEEP_ROOM_ID, sid, "30m", ts, te, round(len(mms) / 30.0, 3),
        stage_label, json.dumps(_ratio(stage_ct, SLEEP_STAGES), ensure_ascii=False), _r3(_mean(conf)),
        json.dumps(_ratio(status_ct, ("absent", "awake", "asleep")), ensure_ascii=False),
        _r3(_mean(tosses)), _r3(max(tosses)), _r3(_p90(tosses)), _toss_events(tosses),
        json.dumps(_ratio(tl_ct, ("calm", "slight", "moderate")), ensure_ascii=False),
        _r1(_mean(hr)), _r1(min(hr) if hr else None), _r1(max(hr) if hr else None), _r1(_pstd(hr) if hr else None), _r3(_mean(hrc)),
        _r1(_mean(br)), _r1(min(br) if br else None), _r1(max(br) if br else None), _r1(_pstd(br) if br else None),
        round(snore_ct / len(asleep), 3) if asleep else None,
        _r2(_mean([mm["temp"] for mm in mms])), _r2(_mean([mm["lux"] for mm in mms])), _r2(_mean([mm["noise"] for mm in mms])),
        summary,
    )


def _sleep_summary(ts, stage_label, hr, br, tosses, snore_ct, asleep_n) -> str:
    hhmm = ts[11:16]
    hrm, brm, tm = _mean(hr), _mean(br), _mean(tosses)
    tl = "적음" if (tm or 0) < 0.2 else ("보통" if (tm or 0) < 0.4 else "많음")
    parts = [f"{hhmm} 구간 주로 {STAGE_KR.get(stage_label, stage_label)}"]
    if hrm is not None:
        parts.append(f"평균 심박 {hrm:.0f}bpm·호흡 {brm:.0f}rpm")
    parts.append(f"뒤척임 {tl}")
    if asleep_n and snore_ct:
        parts.append(f"코골이 {round(snore_ct / asleep_n * 100)}%")
    return ", ".join(parts) + "."


def _group_30m(minutes: list[dict]) -> list[tuple]:
    groups: dict[int, list] = {}
    for mm in minutes:
        groups.setdefault((mm["clock"] // 30) * 30, []).append(mm)
    return sorted(groups.items())


def _sleep_score(eff: float, asleep_min: int, waso: int, deep_ratio: float, rem_ratio: float) -> float:
    dur_h = asleep_min / 60.0
    s = 100.0
    s -= max(0.0, 0.85 - eff) * 100 * 0.5      # 효율 낮음
    s -= max(0.0, 7.5 - dur_h) * 6             # 수면 부족
    s -= max(0, waso - 2) * 3                   # 잦은 각성
    s -= max(0.0, 0.13 - deep_ratio) * 100 * 0.6   # 깊은수면 부족
    s -= max(0.0, 0.18 - rem_ratio) * 100 * 0.3    # 렘 부족
    return round(_clampf(s, 40, 100), 1)


def _build_sleep_reports(cur: sqlite3.Cursor, metas: list[dict]) -> int:
    daily = []
    for m in metas:
        metrics = {
            "score": m["score"], "tib_min": m["tib_min"], "asleep_min": m["asleep_min"],
            "efficiency": m["efficiency"], "latency_min": m["latency_min"], "waso": m["waso"],
            "stage_ratio": {"light": m["light_ratio"], "deep": m["deep_ratio"],
                            "rem": m["rem_ratio"]},  # 수면시간 대비 비율(합≈1)
            "hr_mean": m["hr_mean"], "br_mean": m["br_mean"],
            "snore_ratio": m["snore_ratio"], "toss_events": m["toss_events"],
        }
        daily.append((SLEEP_USER_ID, "daily", m["night_date"].strftime("%Y-%m-%d"), m["session_id"],
                      json.dumps(metrics, ensure_ascii=False), None))
    cur.executemany(
        "INSERT INTO sleep_report (user_id,period,period_start,session_id,metrics,report_text)"
        " VALUES (?,?,?,?,?,?)", daily,
    )

    weeks: dict[date, list] = defaultdict(list)
    for m in metas:
        monday = m["night_date"] - timedelta(days=m["night_date"].weekday())
        weeks[monday].append(m)
    weekly = []
    for monday, ms in sorted(weeks.items()):
        ms = sorted(ms, key=lambda x: x["night_date"])
        metrics = {
            "nights": len(ms),
            "avg_asleep_min": round(_mean([x["asleep_min"] for x in ms])),
            "avg_efficiency": round(_mean([x["efficiency"] for x in ms]), 3),
            "avg_score": round(_mean([x["score"] for x in ms]), 1),
            "avg_deep_ratio": round(_mean([x["deep_ratio"] for x in ms]), 3),
            "avg_rem_ratio": round(_mean([x["rem_ratio"] for x in ms]), 3),
            "avg_snore_ratio": round(_mean([x["snore_ratio"] for x in ms if x["snore_ratio"] is not None] or [0]), 3),
            "scores": [x["score"] for x in ms],
        }
        weekly.append((SLEEP_USER_ID, "weekly", monday.strftime("%Y-%m-%d"), None,
                       json.dumps(metrics, ensure_ascii=False), None))
    cur.executemany(
        "INSERT INTO sleep_report (user_id,period,period_start,session_id,metrics,report_text)"
        " VALUES (?,?,?,?,?,?)", weekly,
    )
    return len(daily) + len(weekly)


def generate_sleep(conn: sqlite3.Connection, devices: list[dict], rng: random.Random) -> dict[str, int]:
    radar_id = next((int(d["id"], 16) for d in devices if d.get("class") == "srs_r4sn"), None)
    station_id = next((int(d["id"], 16) for d in devices if d.get("class") == "wave_station"), None)
    start_date = ANCHOR_DATE - timedelta(days=POWER_DAYS)
    cur = conn.cursor()

    stat_rows: list[tuple] = []
    metas: list[dict] = []
    for di in range(POWER_DAYS):
        D = start_date + timedelta(days=di)
        D0 = datetime(D.year, D.month, D.day)
        weekend = D.weekday() >= 5
        heat = _clampf(0.30 + 0.60 * (di / max(1, POWER_DAYS - 1)) + rng.uniform(-0.12, 0.12), 0.0, 1.0)

        bedtime, onset, wake, out = _sleep_night_bounds(weekend, rng)
        hypno = _build_hypnogram(onset, wake, rng)
        minutes = _night_minutes(D0, bedtime, onset, wake, out, hypno, heat, rng)

        asleep = [mm for mm in minutes if mm["status"] == "asleep"]
        tib_s, asleep_s = len(minutes) * 60, len(asleep) * 60
        eff = asleep_s / tib_s if tib_s else 0.0
        stage_secs = {s: 0 for s in SLEEP_STAGES}
        for mm in minutes:
            stage_secs[mm["stage"]] += 60
        hr_vals = [mm["hr"] for mm in asleep if mm["hr"] is not None]
        br_vals = [mm["br"] for mm in asleep if mm["br"] is not None]
        snore_ct = sum(1 for mm in asleep if mm["snore"])
        toss_ev = _toss_events([mm["toss"] for mm in minutes])
        onset_dt = D0 + timedelta(minutes=onset)
        wake_dt = D0 + timedelta(minutes=wake)

        cur.execute(
            "INSERT INTO sleep_session (user_id,room_id,radar_id,station_id,night_date,onset,final_wake,"
            "time_in_bed_s,asleep_total_s,efficiency,stage_totals,toss_events,hr_mean,br_mean,snore_ratio)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (SLEEP_USER_ID, SLEEP_ROOM_ID, radar_id, station_id, D.strftime("%Y-%m-%d"),
             onset_dt.strftime("%Y-%m-%d %H:%M:00"), wake_dt.strftime("%Y-%m-%d %H:%M:00"),
             tib_s, asleep_s, round(eff, 3), json.dumps(stage_secs, ensure_ascii=False), toss_ev,
             _r1(_mean(hr_vals)), _r1(_mean(br_vals)),
             round(snore_ct / len(asleep), 3) if asleep else None),
        )
        sid = cur.lastrowid

        for mm in minutes:
            stat_rows.append(_stat_row_1m(sid, D0, mm))
        for win_idx, win_mms in _group_30m(minutes):
            stat_rows.append(_stat_row_30m(sid, D0, win_idx, win_mms))

        # WASO(수면 중 각성 에피소드 수)
        waso, prev = 0, None
        for mm in minutes:
            if onset <= mm["clock"] < wake:
                if mm["stage"] == "awake" and prev != "awake":
                    waso += 1
                prev = mm["stage"]
        am = max(1, asleep_s / 60)
        metas.append({
            "session_id": sid, "night_date": D, "score": _sleep_score(
                eff, asleep_s // 60, waso, stage_secs["deep"] / 60 / am, stage_secs["rem"] / 60 / am),
            "tib_min": tib_s // 60, "asleep_min": asleep_s // 60, "efficiency": round(eff, 3),
            "latency_min": onset - bedtime, "waso": waso,
            "deep_ratio": round(stage_secs["deep"] / 60 / am, 3),
            "rem_ratio": round(stage_secs["rem"] / 60 / am, 3),
            "light_ratio": round(stage_secs["light"] / 60 / am, 3),
            "awake_ratio": round(stage_secs["awake"] / 60 / (tib_s / 60 or 1), 3),
            "hr_mean": _r1(_mean(hr_vals)), "br_mean": _r1(_mean(br_vals)),
            "snore_ratio": round(snore_ct / len(asleep), 3) if asleep else None,
            "toss_events": toss_ev,
        })

    cur.executemany(_SLEEP_STAT_INSERT, stat_rows)
    n_reports = _build_sleep_reports(cur, metas)
    conn.commit()
    return {
        "sleep_session": len(metas),
        "sleep_stat": len(stat_rows),
        "sleep_report": n_reports,
    }


# ---------------------------------------------------------------------------
# 벡터(RAG) 테이블. vec0 모듈(sqlite-vec 확장)이 필요하므로 SCHEMA_SQL 과 분리.
# 임베딩은 채우지 않고 테이블만 만든다.
# ---------------------------------------------------------------------------
VEC_SCHEMA_SQL = """
CREATE VIRTUAL TABLE vec_sleep_stat USING vec0 (
    stat_id   INTEGER PRIMARY KEY,
    embedding float[768]
);

CREATE VIRTUAL TABLE vec_sleep_report USING vec0 (
    report_id INTEGER PRIMARY KEY,
    embedding float[768]
);

CREATE VIRTUAL TABLE vec_power_report USING vec0 (
    report_id INTEGER PRIMARY KEY,
    embedding float[768]
);
"""


def build_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(SCHEMA_SQL)


def create_vector_tables(conn: sqlite3.Connection) -> int:
    """sqlite-vec 확장을 로드해 벡터 테이블을 만든다(임베딩은 비움).

    확장이 없으면 건너뛴다. `uv run --with sqlite-vec scripts/gen_mock_data.py`
    로 실행하면 실제로 생성된다.
    """
    try:
        import sqlite_vec  # type: ignore
    except ImportError:
        print("  (sqlite-vec 미설치: 벡터 테이블 생략 - uv run --with sqlite-vec 로 실행)")
        return 0
    try:
        conn.enable_load_extension(True)
        sqlite_vec.load(conn)
        conn.enable_load_extension(False)
    except (AttributeError, sqlite3.OperationalError) as exc:
        print(f"  (확장 로드 불가: 벡터 테이블 생략 - {exc})")
        return 0
    conn.executescript(VEC_SCHEMA_SQL)
    conn.commit()
    return 3


def populate(conn: sqlite3.Connection, devices: list[dict]) -> dict[str, int]:
    cur = conn.cursor()

    cur.executemany(
        "INSERT INTO user (id, name, created_at) VALUES (?, ?, ?)",
        [(uid, name, ANCHOR_CREATED_AT) for uid, name in USERS],
    )

    cur.executemany(
        "INSERT INTO room (id, name, description) VALUES (?, ?, ?)",
        ROOMS,
    )

    cur.executemany(
        "INSERT INTO room_user_map (room_id, user_id) VALUES (?, ?)",
        ROOM_USER_MAP,
    )

    device_rows = []
    device_room_rows = []
    device_user_rows = []
    for dev in devices:
        hex_id = dev["id"]
        if not hex_id:
            raise ValueError(f"id 가 비어 있는 장치: {dev.get('name')}")
        dev_id = int(hex_id, 16)  # json 의 16진수 id 를 정수 PK 로 사용

        device_rows.append(
            (dev_id, dev["name"], dev.get("description", ""), dev["class"], 0)
        )
        device_room_rows.append((dev_id, infer_room_id(dev)))
        # 가정: 집안 장치는 두 사용자가 공유.
        for uid, _ in USERS:
            device_user_rows.append((dev_id, uid))

    cur.executemany(
        "INSERT INTO device (id, name, description, class, archived) VALUES (?, ?, ?, ?, ?)",
        device_rows,
    )
    cur.executemany(
        "INSERT INTO device_room_map (device_id, room_id) VALUES (?, ?)",
        device_room_rows,
    )
    cur.executemany(
        "INSERT INTO device_user_map (device_id, user_id) VALUES (?, ?)",
        device_user_rows,
    )

    cur.executemany(
        "INSERT INTO gesture_set (id, name, archived) VALUES (?, ?, ?)",
        GESTURE_SETS,
    )

    conn.commit()

    return {
        "user": len(USERS),
        "room": len(ROOMS),
        "room_user_map": len(ROOM_USER_MAP),
        "device": len(device_rows),
        "device_room_map": len(device_room_rows),
        "device_user_map": len(device_user_rows),
        "gesture_set": len(GESTURE_SETS),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="WaveHome 목업 DB 생성기")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT, help="출력 SQLite 파일 경로")
    parser.add_argument("--device-list", type=Path, default=DEFAULT_DEVICE_LIST, help="device_list.json 경로")
    args = parser.parse_args()

    devices = load_devices(args.device_list)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.out.exists():
        args.out.unlink()  # 재실행 시 깨끗하게 다시 생성

    rng = random.Random(SEED)

    conn = sqlite3.connect(args.out)
    try:
        conn.execute("PRAGMA foreign_keys = ON")
        build_schema(conn)
        counts = populate(conn, devices)
        counts.update(generate_power(conn, devices, rng))
        counts.update(generate_sleep(conn, devices, random.Random(SEED + 1)))
        counts["vec tables"] = create_vector_tables(conn)
        # FK 무결성 확인
        violations = conn.execute("PRAGMA foreign_key_check").fetchall()
        if violations:
            raise RuntimeError(f"FK 위반 발생: {violations}")
    finally:
        conn.close()

    print(f"목업 DB 생성 완료: {args.out}")
    for table, n in counts.items():
        print(f"  {table:16} {n:4d} rows")


if __name__ == "__main__":
    main()
