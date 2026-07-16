"""demo/sleep.md 의 규칙을 그대로 구현한 수면 1분/30분 통계 + 세션 생성기.

레이더는 초당 20샘플 -> 1초 행으로 이미 집계된 값을 낸다(참고 CSV). 이 모듈은 그 "1초 행"을
직접 저장하지 않고, 1분 단위 통계치를 곧바로 확률모델로 합성한다(결과적으로 동일한 분포의
1분 집계를 만들되 초 단위 중간 산출물은 만들지 않는다 - 성능/저장 공간상 합리적 축약).
"""

from __future__ import annotations

import json
import math
import random
from datetime import datetime, timedelta

from .sleep_scenario import SCENARIO
from .timeutil import MONTH_START, fmt_dt

SEED = 20260601 + 500

STAGE_ORDER = ("deep", "light", "rem")


def _clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def _parse_hm(s: str) -> tuple[int, int]:
    h, m = s.split(":")
    return int(h), int(m)


def compute_bed_wake(date_str: str, bed_str: str, wake_str: str, tib_hours: float) -> tuple[datetime, datetime]:
    d = datetime.strptime(date_str, "%Y-%m-%d")
    bh, bm = _parse_hm(bed_str)
    bed_date = d + timedelta(days=1) if bh < 12 else d
    bed_dt = bed_date.replace(hour=bh, minute=bm)
    wake_dt = bed_dt + timedelta(hours=tib_hours)
    return bed_dt, wake_dt


def target_efficiency(score: int) -> float:
    return _clamp(0.98 - (100 - score) / 100 * 0.35, 0.80, 0.98)


def quality_scale(score: int) -> float:
    """점수가 낮을수록(나쁜 밤일수록) 커지는 배율(뒤척임/각성 스케일링용)."""
    return _clamp((100 - score) / 40.0, 0.4, 2.0)


def heat_factor_for_day(date_str: str) -> float:
    day_index = (datetime.strptime(date_str, "%Y-%m-%d").date() - MONTH_START).days
    return _clamp(0.25 + 0.60 * (day_index / 29.0), 0.0, 1.0)


def _stage_sequence_for_asleep(asleep_minutes: int, deep_pct: int, rem_pct: int, rng: random.Random) -> list[str]:
    if asleep_minutes <= 0:
        return []
    n_cycles = max(1, math.ceil(asleep_minutes / 90))
    cycle_lengths = [asleep_minutes // n_cycles] * n_cycles
    for i in range(asleep_minutes - sum(cycle_lengths)):
        cycle_lengths[i] += 1

    def deep_w(i: int) -> float:
        frac = i / (n_cycles - 1) if n_cycles > 1 else 0.0
        return 1.6 + (0.5 - 1.6) * frac

    def rem_w(i: int) -> float:
        frac = i / (n_cycles - 1) if n_cycles > 1 else 0.0
        return 0.5 + (1.6 - 0.5) * frac

    target_deep = round(asleep_minutes * deep_pct / 100)
    target_rem = round(asleep_minutes * rem_pct / 100)
    target_deep = min(target_deep, asleep_minutes)
    target_rem = min(target_rem, asleep_minutes - target_deep)

    deep_weights = [deep_w(i) for i in range(n_cycles)]
    rem_weights = [rem_w(i) for i in range(n_cycles)]
    deep_wsum, rem_wsum = sum(deep_weights), sum(rem_weights)

    deep_alloc = [round(target_deep * w / deep_wsum) if deep_wsum else 0 for w in deep_weights]
    rem_alloc = [round(target_rem * w / rem_wsum) if rem_wsum else 0 for w in rem_weights]

    # 반올림 오차 보정(마지막 사이클에 몰아줌)
    deep_alloc[-1] += target_deep - sum(deep_alloc)
    rem_alloc[-1] += target_rem - sum(rem_alloc)

    seq: list[str] = []
    for i, length in enumerate(cycle_lengths):
        d_i = max(0, min(deep_alloc[i], length))
        r_i = max(0, min(rem_alloc[i], length - d_i))
        l_i = length - d_i - r_i
        seq.extend(["deep"] * d_i)
        seq.extend(["light"] * l_i)
        seq.extend(["rem"] * r_i)

    # 길이 보정(반올림으로 asleep_minutes 와 어긋나면 light 로 맞춤)
    if len(seq) < asleep_minutes:
        seq.extend(["light"] * (asleep_minutes - len(seq)))
    elif len(seq) > asleep_minutes:
        seq = seq[:asleep_minutes]
    rng.shuffle if False else None  # noqa: no-op, 순서(deep->light->rem)를 사이클 내에서 유지하기 위해 셔플하지 않음
    return seq


def _insert_blips(core_seq: list[str], score: int, asleep_hours: float, rng: random.Random) -> list[str]:
    q = quality_scale(score)
    n_blips = max(0, round(q * asleep_hours * 0.4 + rng.uniform(-0.3, 0.3)))
    seq = list(core_seq)
    for _ in range(n_blips):
        if len(seq) < 20:
            break
        pos = rng.randint(10, max(10, len(seq) - 10))
        blip_len = rng.randint(1, 3)
        seq[pos:pos] = ["awake"] * blip_len
    return seq


def _status_of(label: str) -> str:
    if label == "absent":
        return "absent"
    if label == "awake":
        return "awake"
    return "asleep"


def _toss_base(label: str) -> float:
    return {"deep": 0.015, "light": 0.04, "rem": 0.05, "awake": 0.4, "absent": 0.0}[label]


def _toss_ratio(mean: float) -> dict[str, float]:
    if mean < 0.05:
        return {"calm": 0.95, "slight": 0.04, "moderate": 0.01}
    if mean < 0.15:
        return {"calm": 0.80, "slight": 0.15, "moderate": 0.05}
    if mean < 0.30:
        return {"calm": 0.55, "slight": 0.30, "moderate": 0.15}
    return {"calm": 0.30, "slight": 0.35, "moderate": 0.35}


def _hr_base(label: str) -> float:
    return {"deep": 57.0, "light": 60.0, "rem": 63.0, "awake": 68.0, "absent": 0.0}[label]


def _br_base(label: str) -> float:
    return {"deep": 13.5, "light": 15.0, "rem": 16.0, "awake": 18.0, "absent": 0.0}[label]


def _env_temp_curve(minute_idx: int, is_heatwave: bool, heat: float) -> float:
    outdoor_base = 24.0 + 5.5 * heat
    floor = outdoor_base - (2.5 if is_heatwave else 5.0)
    tau = 45.0 if is_heatwave else 30.0
    return floor + (outdoor_base - floor) * math.exp(-minute_idx / tau)


def _env_lux(minute_idx: int, total_minutes: int, final_brightness: int, wake_ramp: bool) -> float:
    ramp_window = 30
    if wake_ramp and minute_idx >= total_minutes - ramp_window:
        prog = (minute_idx - (total_minutes - ramp_window)) / ramp_window
        return final_brightness + (200 - final_brightness) * _clamp(prog, 0.0, 1.0)
    return final_brightness


def generate_night(
    scenario_row: tuple,
    user_id: int,
    room_id: int,
    radar_id: int,
    station_id: int,
    rng: random.Random,
) -> dict:
    date_str, bed_str, wake_str, tib_hours, score, deep_pct, rem_pct, note = scenario_row
    bed_dt, wake_dt = compute_bed_wake(date_str, bed_str, wake_str, tib_hours)
    tib_minutes = round(tib_hours * 60)
    efficiency = target_efficiency(score)
    asleep_minutes = round(tib_minutes * efficiency)
    trans_minutes = tib_minutes - asleep_minutes

    pre_trans = round(trans_minutes * 0.6)
    post_trans = trans_minutes - pre_trans
    pre_absent = max(1, round(pre_trans * 0.7))
    pre_awake = max(0, pre_trans - pre_absent)
    post_absent = max(1, round(post_trans * 0.7))
    post_awake = max(0, post_trans - post_absent)

    core_stage_seq = _stage_sequence_for_asleep(asleep_minutes, deep_pct, rem_pct, rng)
    core_seq = _insert_blips(core_stage_seq, score, tib_hours, rng)

    full_seq = (
        ["absent"] * pre_absent
        + ["awake"] * pre_awake
        + core_seq
        + ["awake"] * post_awake
        + ["absent"] * post_absent
    )
    total_minutes = len(full_seq)

    is_heatwave = "폭염" in note
    heat = heat_factor_for_day(date_str)
    q = quality_scale(score)

    minute_rows: list[dict] = []
    for i, label in enumerate(full_seq):
        status = _status_of(label)
        toss_base = _toss_base(label) * (0.7 + 0.3 * q)
        is_blip = label == "awake" and 0 < i < total_minutes - 1 and full_seq[i - 1] in ("deep", "light", "rem")
        toss_mean = _clamp(rng.lognormvariate(math.log(max(toss_base, 1e-4)), 0.6), 0.0, 1.0)
        if is_blip:
            toss_mean = _clamp(rng.uniform(0.45, 0.85), 0.0, 1.0)
        toss_max = _clamp(toss_mean * rng.uniform(2.0, 9.0), toss_mean, 1.0)
        toss_p90 = toss_mean + 0.7 * (toss_max - toss_mean)
        toss_events = 1 if toss_max >= 0.5 else 0
        ratio = _toss_ratio(toss_mean)

        hr_base = _hr_base(label) + (2.0 if is_heatwave and status != "absent" else 0.0)
        hr_mean = hr_base + rng.gauss(0, 1.2) if status != "absent" else None
        hr_std = 1.5 + (3.0 if is_blip else 0.0)
        br_base = _br_base(label)
        br_mean = br_base + rng.gauss(0, 0.6) if status != "absent" else None

        env_temp = _env_temp_curve(i, is_heatwave, heat)
        env_lux = _env_lux(i, total_minutes, 10, True)
        env_noise = _clamp(22 + toss_mean * 40 + rng.gauss(0, 2), 15, 65)

        snore_base = {"deep": 0.35, "light": 0.15, "rem": 0.05, "awake": 0.0, "absent": 0.0}[label]
        temp_penalty = max(0.0, env_temp - 25.0) * 0.08
        toss_penalty = -toss_mean * 0.3
        snore_ratio = _clamp(snore_base + temp_penalty + toss_penalty + rng.gauss(0, 0.05), 0.0, 1.0)

        ts = bed_dt + timedelta(minutes=i)
        status_ratio = {"absent": 0.0, "awake": 0.0, "asleep": 0.0}
        status_ratio[status] = 1.0
        if 0 < i < total_minutes - 1 and _status_of(full_seq[i - 1]) != status:
            status_ratio[status] = 0.9
            status_ratio[_status_of(full_seq[i - 1])] = 0.1

        minute_rows.append(
            {
                "ts": ts,
                "coverage": round(rng.uniform(0.95, 1.0), 3) if rng.random() > 0.05 else round(rng.uniform(0.85, 0.95), 3),
                "status": status,
                "stage_label": label,
                "status_ratio": status_ratio,
                "toss_mean": round(toss_mean, 4),
                "toss_max": round(toss_max, 4),
                "toss_p90": round(toss_p90, 4),
                "toss_events": toss_events,
                "toss_ratio": {k: round(v, 3) for k, v in ratio.items()},
                "hr_mean": round(hr_mean, 1) if hr_mean is not None else None,
                "hr_min": round(hr_mean - rng.uniform(1, 3), 1) if hr_mean is not None else None,
                "hr_max": round(hr_mean + rng.uniform(1, 3) + (6 if is_blip else 0), 1) if hr_mean is not None else None,
                "hr_std": round(hr_std, 2),
                "hr_confidence": round(rng.uniform(0.85, 0.99), 3) if status != "absent" else None,
                "br_mean": round(br_mean, 1) if br_mean is not None else None,
                "br_min": round(br_mean - rng.uniform(0.3, 1.0), 1) if br_mean is not None else None,
                "br_max": round(br_mean + rng.uniform(0.3, 1.5), 1) if br_mean is not None else None,
                "br_std": round(0.5 + (1.0 if is_blip else 0.0), 2),
                "snore_ratio": round(snore_ratio, 3),
                "env_temp": round(env_temp, 2),
                "env_lux": round(env_lux, 1),
                "env_noise": round(env_noise, 1),
            }
        )

    session = _build_session(minute_rows, bed_dt, wake_dt, user_id, room_id, radar_id, station_id, date_str)
    stat_1m = _build_1m_rows(minute_rows, user_id, room_id)
    stat_30m = _build_30m_rows(minute_rows, user_id, room_id)
    return {"date": date_str, "session": session, "stat_1m": stat_1m, "stat_30m": stat_30m}


def _build_session(minute_rows, bed_dt, wake_dt, user_id, room_id, radar_id, station_id, night_date: str) -> dict:
    asleep_rows = [r for r in minute_rows if r["status"] == "asleep"]
    stage_totals: dict[str, int] = {}
    for r in minute_rows:
        stage_totals[r["stage_label"]] = stage_totals.get(r["stage_label"], 0) + 1

    tib_s = int(round((wake_dt - bed_dt).total_seconds()))
    asleep_s = len(asleep_rows) * 60
    hr_vals = [r["hr_mean"] for r in minute_rows if r["hr_mean"] is not None]
    br_vals = [r["br_mean"] for r in minute_rows if r["br_mean"] is not None]
    snore_vals = [r["snore_ratio"] for r in minute_rows if r["status"] != "absent"]
    toss_events_total = sum(r["toss_events"] for r in minute_rows)

    return {
        "user_id": user_id,
        "room_id": room_id,
        "radar_id": radar_id,
        "station_id": station_id,
        "night_date": night_date,
        "onset": fmt_dt(bed_dt),
        "final_wake": fmt_dt(wake_dt),
        "time_in_bed_s": tib_s,
        "asleep_total_s": asleep_s,
        "efficiency": round(asleep_s / tib_s, 4) if tib_s else None,
        "stage_totals": json.dumps({k: v * 60 for k, v in stage_totals.items()}, ensure_ascii=False),
        "toss_events": toss_events_total,
        "hr_mean": round(sum(hr_vals) / len(hr_vals), 1) if hr_vals else None,
        "br_mean": round(sum(br_vals) / len(br_vals), 1) if br_vals else None,
        "snore_ratio": round(sum(snore_vals) / len(snore_vals), 3) if snore_vals else None,
    }


def _build_1m_rows(minute_rows, user_id, room_id) -> list[dict]:
    out = []
    for r in minute_rows:
        out.append(
            {
                "user_id": user_id,
                "room_id": room_id,
                "granularity": "1m",
                "time_start": fmt_dt(r["ts"]),
                "time_end": fmt_dt(r["ts"] + timedelta(minutes=1)),
                "coverage": r["coverage"],
                "stage_label": r["stage_label"],
                "stage_ratio": json.dumps({r["stage_label"]: 1.0}, ensure_ascii=False),
                "stage_confidence": round(random.Random(hash(r["ts"]) & 0xFFFF).uniform(0.8, 0.98), 3),
                "status_ratio": json.dumps(r["status_ratio"], ensure_ascii=False),
                "toss_mean": r["toss_mean"],
                "toss_max": r["toss_max"],
                "toss_p90": r["toss_p90"],
                "toss_events": r["toss_events"],
                "toss_ratio": json.dumps(r["toss_ratio"], ensure_ascii=False),
                "hr_mean": r["hr_mean"],
                "hr_min": r["hr_min"],
                "hr_max": r["hr_max"],
                "hr_std": r["hr_std"],
                "hr_confidence": r["hr_confidence"],
                "br_mean": r["br_mean"],
                "br_min": r["br_min"],
                "br_max": r["br_max"],
                "br_std": r["br_std"],
                "snore_ratio": r["snore_ratio"],
                "env_temp": r["env_temp"],
                "env_lux": r["env_lux"],
                "env_noise": r["env_noise"],
                "summary_text": None,
            }
        )
    return out


def _avg(vals: list[float | None]) -> float | None:
    v = [x for x in vals if x is not None]
    return round(sum(v) / len(v), 2) if v else None


def _build_30m_rows(minute_rows, user_id, room_id) -> list[dict]:
    out = []
    for i in range(0, len(minute_rows), 30):
        chunk = minute_rows[i : i + 30]
        if not chunk:
            continue
        stage_counts: dict[str, int] = {}
        status_counts: dict[str, int] = {"absent": 0, "awake": 0, "asleep": 0}
        toss_ratio_sum = {"calm": 0.0, "slight": 0.0, "moderate": 0.0}
        for r in chunk:
            stage_counts[r["stage_label"]] = stage_counts.get(r["stage_label"], 0) + 1
            status_counts[r["status"]] += 1
            for k in toss_ratio_sum:
                toss_ratio_sum[k] += r["toss_ratio"][k]
        n = len(chunk)
        dominant_stage = max(stage_counts, key=stage_counts.get)
        dominant_status = max(status_counts, key=status_counts.get)
        toss_ratio_avg = {k: round(v / n, 3) for k, v in toss_ratio_sum.items()}
        calm_pct = round(toss_ratio_avg["calm"] * 100)

        start = chunk[0]["ts"]
        end = chunk[-1]["ts"] + timedelta(minutes=1)
        summary_text = (
            f"{start.strftime('%H:%M')}~{end.strftime('%H:%M')} 대부분 {dominant_stage}, "
            f"뒤척임 {'낮음' if calm_pct >= 80 else '보통' if calm_pct >= 50 else '높음'}(calm {calm_pct}%)"
        )

        out.append(
            {
                "user_id": user_id,
                "room_id": room_id,
                "granularity": "30m",
                "time_start": fmt_dt(start),
                "time_end": fmt_dt(end),
                "coverage": _avg([r["coverage"] for r in chunk]),
                "stage_label": dominant_stage,
                "stage_ratio": json.dumps({k: round(v / n, 3) for k, v in stage_counts.items()}, ensure_ascii=False),
                "stage_confidence": _avg([None]) or 0.9,
                "status_ratio": json.dumps({k: round(v / n, 3) for k, v in status_counts.items()}, ensure_ascii=False),
                "toss_mean": _avg([r["toss_mean"] for r in chunk]),
                "toss_max": max(r["toss_max"] for r in chunk),
                "toss_p90": _avg([r["toss_p90"] for r in chunk]),
                "toss_events": sum(r["toss_events"] for r in chunk),
                "toss_ratio": json.dumps(toss_ratio_avg, ensure_ascii=False),
                "hr_mean": _avg([r["hr_mean"] for r in chunk]),
                "hr_min": _avg([r["hr_min"] for r in chunk]),
                "hr_max": _avg([r["hr_max"] for r in chunk]),
                "hr_std": _avg([r["hr_std"] for r in chunk]),
                "hr_confidence": _avg([r["hr_confidence"] for r in chunk]),
                "br_mean": _avg([r["br_mean"] for r in chunk]),
                "br_min": _avg([r["br_min"] for r in chunk]),
                "br_max": _avg([r["br_max"] for r in chunk]),
                "br_std": _avg([r["br_std"] for r in chunk]),
                "snore_ratio": _avg([r["snore_ratio"] for r in chunk]),
                "env_temp": _avg([r["env_temp"] for r in chunk]),
                "env_lux": _avg([r["env_lux"] for r in chunk]),
                "env_noise": _avg([r["env_noise"] for r in chunk]),
                "summary_text": summary_text,
            }
        )
    return out


def generate_all_nights(user_id: int, room_id: int, radar_id: int, station_id: int) -> list[dict]:
    rng = random.Random(SEED)
    return [generate_night(row, user_id, room_id, radar_id, station_id, rng) for row in SCENARIO]
