"""전력(power_energy) 30일 물리 시뮬레이션.

계측 가능한 장치는 스마트 플러그(tuya_ep2h) 5개:
- 거실 선풍기(fan), 침실 컴퓨터(pc), 침실 에어컨(aircon),
  부엌 인덕션(induction), 부엌 전자레인지(microwave)

30초 1샘플로 와트를 시뮬레이션한 뒤 5분 버킷으로 적분하고, 5m -> 1h -> 24h 로 달력
롤업, 1h/24h -> 1w(슬라이딩 7일)/1mo(슬라이딩 30일)로 다시 롤업한다. 자세한 시나리오
설명은 mock/power.md 를 참고.
"""

from __future__ import annotations

import random
import sqlite3
from datetime import date, datetime, timedelta

from . import timeutil

RAW_STEP_S = 30  # 실제 플러그 샘플 주기(30초)
BUCKET_MIN = 5
SAMPLES_PER_BUCKET = (BUCKET_MIN * 60) // RAW_STEP_S  # 10
DISCONNECT_PROB = 0.004
ACTIVE_W_THRESHOLD = 10.0
SEED = 20260601

# 6월 1일(월) 기준 특이일: 재택근무 / 폭염 / 저녁 모임
IRREGULAR_DAYS: dict[date, str] = {
    date(2026, 6, 10): "wfh",         # 수요일, 재택근무 - 낮에도 재실
    date(2026, 6, 18): "heatwave",    # 목요일, 이른 폭염 - 에어컨 듀티 급증
    date(2026, 6, 25): "gathering",   # 목요일, 저녁 모임 - 인덕션/선풍기 사용 증가
}

PLUG_APPLIANCES = {
    "aircon": "에어컨",
    "fan": "선풍기",
    "pc": "컴퓨터",
    "induction": "인덕션",
    "microwave": "전자레인지",
}


def _clampf(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def _standby(watt: float, rng: random.Random) -> float:
    return max(0.0, rng.gauss(watt, watt * 0.15))


def heat_factor(day_index: int, rng: random.Random) -> float:
    """초여름(0.25) -> 6월 말(0.85) 로 갈수록 더워진다. 하루 편차 포함."""
    base = 0.25 + 0.60 * (day_index / max(1, timeutil.DAYS_IN_MONTH - 1))
    return _clampf(base + rng.uniform(-0.10, 0.10), 0.0, 1.0)


def occupancy(dt: datetime, irregular: str | None) -> tuple[bool, bool]:
    """(home, bedroom_sleeping). 평일 09~18 부재(재택근무일 제외), 주말 재택, 23~07 취침."""
    weekend = dt.weekday() >= 5
    sleeping = dt.hour >= 23 or dt.hour < 7
    if irregular == "wfh":
        home = True
    else:
        home = True if weekend else (dt.hour < 9 or dt.hour >= 18)
    return home, sleeping


def day_flags(appliance: str, heat: float, irregular: str | None, rng: random.Random) -> dict:
    if appliance == "aircon":
        used = rng.random() < 0.20 + 0.75 * heat
        if irregular == "heatwave":
            used = True
        return {"used": used}
    if appliance == "fan":
        used = rng.random() < 0.45 + 0.45 * heat
        if irregular == "gathering":
            used = True
        return {"used": used}
    if appliance == "pc":
        return {"session": rng.random() < 0.90}
    if appliance == "induction":
        return {
            "breakfast": rng.random() < 0.55,
            "dinner": rng.random() < 0.80 or irregular == "gathering",
        }
    if appliance == "microwave":
        return {
            "lunch": rng.random() < 0.45,
            "snack": rng.random() < 0.35,
            "dinner": rng.random() < 0.55 or irregular == "gathering",
        }
    return {}


def _watt_aircon(dt: datetime, heat: float, flags: dict, irregular: str | None, rng: random.Random) -> float:
    """침실 에어컨 - 저녁~새벽(수면) 중심, 낮잠/재택 시간대도 소폭 포함."""
    if not flags["used"]:
        return _standby(2.0, rng)
    h = dt.hour
    night_window = h >= 21 or h < 8  # 취침 전 냉방 ~ 아침
    day_window = irregular == "wfh" and 12 <= h < 18
    if not (night_window or day_window):
        return _standby(2.0, rng)

    duty = _clampf(0.30 + 0.55 * heat, 0.0, 0.95)
    if irregular == "heatwave":
        duty = _clampf(duty + 0.20, 0.0, 0.98)
    # 취침 중(00~06시)에는 컴프레서 듀티를 낮추고 저소음 송풍 위주로 유지(수면 쾌적).
    if 0 <= h < 6:
        duty *= 0.6
    if rng.random() < duty:
        return _clampf(rng.gauss(870, 40), 650, 1100)  # 컴프레서 가동
    return _clampf(rng.gauss(50, 6), 35, 75)  # 실내기 송풍


def _watt_fan(dt: datetime, flags: dict, home: bool, irregular: str | None, rng: random.Random) -> float:
    if not flags["used"]:
        return 0.0
    weekend = dt.weekday() >= 5
    elig = (11 <= dt.hour < 23) if weekend else (18 <= dt.hour < 23)
    if irregular == "gathering":
        elig = elig or (17 <= dt.hour < 23)
    if home and elig and rng.random() < 0.80:
        return _clampf(rng.gauss(45, 5), 30, 65)
    return 0.0


def _watt_pc(dt: datetime, flags: dict, home: bool, rng: random.Random) -> float:
    h = dt.hour + dt.minute / 60.0
    win = (13.0 <= h < 23.5) if dt.weekday() >= 5 else (19.0 <= h < 23.5)
    if flags["session"] and home and win:
        if rng.random() < 0.15:
            return _clampf(rng.gauss(210, 30), 140, 320)  # 로드(게임/작업)
        return _clampf(rng.gauss(80, 8), 55, 120)  # 아이들
    return _standby(4.0, rng)


def _watt_induction(dt: datetime, flags: dict, home: bool, irregular: str | None, rng: random.Random) -> float:
    """부엌 인덕션 - 조리 시간대에만 짧고 굵게(1000~1800W), 그 외 0W(대기전력 없음)."""
    h = dt.hour + dt.minute / 60.0
    weekend = dt.weekday() >= 5
    breakfast_win = (8.0 <= h < 9.5) if weekend else (7.0 <= h < 8.0)
    dinner_win = (18.0 <= h < 20.0) if weekend else (18.0 <= h < 19.5)
    if irregular == "gathering":
        dinner_win = 18.0 <= h < 21.0
    if not home:
        return 0.0
    if flags.get("breakfast") and breakfast_win and rng.random() < 0.6:
        return _clampf(rng.gauss(1150, 150), 700, 1700)
    if flags.get("dinner") and dinner_win and rng.random() < 0.7:
        watt = 1500 if irregular == "gathering" else 1200
        return _clampf(rng.gauss(watt, 200), 700, 1900)
    return 0.0


def _watt_microwave(dt: datetime, flags: dict, home: bool, irregular: str | None, rng: random.Random) -> float:
    """부엌 전자레인지 - 짧은 가열 버스트(정격 ~1100W), 그 외 0W."""
    h = dt.hour + dt.minute / 60.0
    weekend = dt.weekday() >= 5
    lunch_win = (12.0 <= h < 13.5) if weekend else (12.0 <= h < 13.0)
    snack_win = (15.5 <= h < 16.5)
    dinner_win = (18.5 <= h < 20.5) if weekend else (18.5 <= h < 19.5)
    if irregular == "gathering":
        dinner_win = 18.0 <= h < 21.5
    if not home:
        return 0.0
    # 버스트는 드물게 — 한 샘플만 ~1100W 근처로 올라가게 한다.
    if flags.get("lunch") and lunch_win and rng.random() < 0.18:
        return _clampf(rng.gauss(1100, 80), 900, 1250)
    if flags.get("snack") and snack_win and rng.random() < 0.12:
        return _clampf(rng.gauss(1050, 90), 850, 1200)
    if flags.get("dinner") and dinner_win and rng.random() < 0.22:
        return _clampf(rng.gauss(1100, 70), 900, 1250)
    return 0.0


def simulate_plug_5m(appliance: str, rng: random.Random) -> list[tuple[str, float, float, int]]:
    """(time_start, energy_wh, coverage, sample_count) 5분 버킷 목록을 생성한다."""
    out: list[tuple[str, float, float, int]] = []
    dt = datetime.combine(timeutil.MONTH_START, datetime.min.time())
    end = datetime.combine(timeutil.MONTH_END + timedelta(days=1), datetime.min.time())

    day_cache: dict[date, dict] = {}
    while dt < end:
        d = dt.date()
        if d not in day_cache:
            irregular = IRREGULAR_DAYS.get(d)
            heat = heat_factor((d - timeutil.MONTH_START).days, rng)
            flags = day_flags(appliance, heat, irregular, rng)
            day_cache[d] = {"irregular": irregular, "heat": heat, "flags": flags}
        ctx = day_cache[d]
        home, sleeping = occupancy(dt, ctx["irregular"])

        bucket_start = dt
        energy_wh = 0.0
        cov_samples = 0
        for _ in range(SAMPLES_PER_BUCKET):
            if rng.random() < DISCONNECT_PROB:
                dt += timedelta(seconds=RAW_STEP_S)
                continue
            if appliance == "aircon":
                w = _watt_aircon(dt, ctx["heat"], ctx["flags"], ctx["irregular"], rng)
            elif appliance == "fan":
                w = _watt_fan(dt, ctx["flags"], home, ctx["irregular"], rng)
            elif appliance == "pc":
                w = _watt_pc(dt, ctx["flags"], home, rng)
            elif appliance == "microwave":
                w = _watt_microwave(dt, ctx["flags"], home, ctx["irregular"], rng)
            else:
                w = _watt_induction(dt, ctx["flags"], home, ctx["irregular"], rng)
            energy_wh += w * (RAW_STEP_S / 3600.0)
            cov_samples += 1
            dt += timedelta(seconds=RAW_STEP_S)

        coverage = cov_samples / SAMPLES_PER_BUCKET
        if energy_wh > 0.0 or coverage < 1.0:
            out.append((timeutil.fmt_dt(bucket_start), round(energy_wh, 4), round(coverage, 4), cov_samples))
    return out


def _rollup(rows: dict[int, list[tuple[str, float, float, int]]], key_fn) -> dict[int, list[tuple[str, float, float, int]]]:
    """디바이스별 5m(or 하위) 행을 상위 granularity 로 합산(coverage 는 가중평균)."""
    out: dict[int, list[tuple[str, float, float, int]]] = {}
    for dev_id, rows_list in rows.items():
        buckets: dict[str, list] = {}
        for ts, e, cov, sc in rows_list:
            key = key_fn(ts)
            b = buckets.setdefault(key, [0.0, 0.0, 0])
            b[0] += e
            b[1] += cov * sc
            b[2] += sc
        out[dev_id] = [(ts, round(e, 4), round((covw / sc) if sc else 0.0, 4), sc) for ts, (e, covw, sc) in buckets.items()]
    return out


def _aggregate(rows: dict[int, list[tuple[str, float, float, int]]]) -> list[tuple[str, float, float, int]]:
    """플러그 전체 합산(device_id=NULL). coverage 는 가중평균."""
    buckets: dict[str, list] = {}
    for rows_list in rows.values():
        for ts, e, cov, sc in rows_list:
            b = buckets.setdefault(ts, [0.0, 0.0, 0])
            b[0] += e
            b[1] += cov * sc
            b[2] += sc
    return [(ts, round(e, 4), round((covw / sc) if sc else 0.0, 4), sc) for ts, (e, covw, sc) in buckets.items()]


def _sliding_window_rollup(
    day_rows: dict[int | None, dict[str, tuple[float, float, int]]],
    dates: list[date],
    window_days: int,
    granularity: str,
) -> list[tuple[int | None, str, float, float, int]]:
    """24h 행(day_rows[dev][date_str] = (energy, coverage, sample_count))을 슬라이딩 창으로 합산."""
    out = []
    for dev_id, by_date in day_rows.items():
        for i in range(len(dates)):
            if i < window_days - 1:
                continue
            window = dates[i - window_days + 1 : i + 1]
            keys = [timeutil.fmt_date(d) for d in window]
            if not all(k in by_date for k in keys):
                continue
            total_e = sum(by_date[k][0] for k in keys)
            avg_cov = sum(by_date[k][1] for k in keys) / len(keys)
            total_sc = sum(by_date[k][2] for k in keys)
            out.append((dev_id, timeutil.fmt_date(window[0]), round(total_e, 4), round(avg_cov, 4), total_sc))
    return out


def generate_power_energy(conn: sqlite3.Connection, plug_pk_by_appliance: dict[str, int]) -> dict[str, int]:
    """플러그별 5m/1h/24h/1w/1mo power_energy 행을 만들어 DB에 넣는다."""
    dates = timeutil.june_dates()
    counts: dict[str, int] = {"5m": 0, "1h": 0, "24h": 0, "1w": 0, "1mo": 0}

    rows_5m: dict[int, list] = {}
    for i, (appliance, dev_id) in enumerate(plug_pk_by_appliance.items()):
        rng = random.Random(SEED + i)
        rows_5m[dev_id] = simulate_plug_5m(appliance, rng)

    agg_5m = _aggregate(rows_5m)

    def to_1h(ts: str) -> str:
        return ts[:13] + ":00:00"

    def to_24h(ts: str) -> str:
        return ts[:10]

    rows_1h = _rollup(rows_5m, to_1h)
    agg_1h_map = _rollup({0: agg_5m}, to_1h)[0]
    rows_24h = _rollup(rows_5m, to_24h)
    agg_24h_map = _rollup({0: agg_5m}, to_24h)[0]

    cur = conn.cursor()

    def insert(dev_id: int | None, gran: str, ts: str, e: float, cov: float, sc: int) -> None:
        cur.execute(
            "INSERT INTO power_energy (device_id, granularity, time_start, energy_wh, coverage, sample_count) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (dev_id, gran, ts, e, cov, sc),
        )
        counts[gran] += 1

    for dev_id, rows_list in rows_5m.items():
        for ts, e, cov, sc in rows_list:
            insert(dev_id, "5m", ts, e, cov, sc)
    for ts, e, cov, sc in agg_5m:
        insert(None, "5m", ts, e, cov, sc)

    for dev_id, rows_list in rows_1h.items():
        for ts, e, cov, sc in rows_list:
            insert(dev_id, "1h", ts, e, cov, sc)
    for ts, e, cov, sc in agg_1h_map:
        insert(None, "1h", ts, e, cov, sc)

    day_rows_by_dev: dict[int | None, dict[str, tuple]] = {}
    for dev_id, rows_list in rows_24h.items():
        for ts, e, cov, sc in rows_list:
            insert(dev_id, "24h", ts, e, cov, sc)
            day_rows_by_dev.setdefault(dev_id, {})[ts] = (e, cov, sc)
    for ts, e, cov, sc in agg_24h_map:
        insert(None, "24h", ts, e, cov, sc)
        day_rows_by_dev.setdefault(None, {})[ts] = (e, cov, sc)

    for dev_id, ts, e, cov, sc in _sliding_window_rollup(day_rows_by_dev, dates, 7, "1w"):
        insert(dev_id, "1w", ts, e, cov, sc)
    for dev_id, ts, e, cov, sc in _sliding_window_rollup(day_rows_by_dev, dates, 30, "1mo"):
        insert(dev_id, "1mo", ts, e, cov, sc)

    conn.commit()
    return counts
