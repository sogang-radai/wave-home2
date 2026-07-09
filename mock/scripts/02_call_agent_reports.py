#!/usr/bin/env python3
"""에이전트(:8501)에 daily/weekly 수면 리포트 + 24h/1w/1mo 전력 리포트를 실호출한다(축소안).

DB에는 쓰지 않고 결과를 mock/ai_reports/*.json 으로만 남긴다(05_load_ai_json_to_db.py가 반영).
전력은 계측 플러그 합산(device_id=NULL)만 실호출한다(약속된 축소 범위: 24h 30 + 1w 24 + 1mo 1 = 55건).
수면은 daily 30 + weekly 24(롤링 7일 창, period_start=창 첫날) = 54건.

실행 전: agent 서버(:8501)가 떠 있어야 한다.
    uvicorn app.main:app --port 8501   (wave-home-agent/)

실행:
    python3 mock/scripts/02_call_agent_reports.py
"""

from __future__ import annotations

import json
import sqlite3
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta
from pathlib import Path

from lib import agent_client, timeutil
from lib.sleep_scenario import SCENARIO

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "mock" / "data" / "mock.db"
OUT_DIR = REPO_ROOT / "mock" / "ai_reports"
OUT_DIR.mkdir(parents=True, exist_ok=True)

MAX_WORKERS = 1
USER_ID = 1

SNAKE_TO_CAMEL_MAP = {
    "user_id": "userId", "room_id": "roomId", "session_id": "sessionId",
    "time_start": "timeStart", "time_end": "timeEnd", "stage_label": "stageLabel",
    "stage_ratio": "stageRatio", "stage_confidence": "stageConfidence", "status_ratio": "statusRatio",
    "toss_mean": "tossMean", "toss_max": "tossMax", "toss_p90": "tossP90", "toss_events": "tossEvents",
    "toss_ratio": "tossRatio", "hr_mean": "hrMean", "hr_min": "hrMin", "hr_max": "hrMax", "hr_std": "hrStd",
    "hr_confidence": "hrConfidence", "br_mean": "brMean", "br_min": "brMin", "br_max": "brMax", "br_std": "brStd",
    "snore_ratio": "snoreRatio", "env_temp": "envTemp", "env_lux": "envLux", "env_noise": "envNoise",
    "night_date": "nightDate", "final_wake": "finalWake", "time_in_bed_s": "timeInBedS",
    "asleep_total_s": "asleepTotalS", "stage_totals": "stageTotals", "radar_id": "radarId",
    "station_id": "stationId", "device_id": "deviceId", "energy_wh": "energyWh", "sample_count": "sampleCount",
}
JSON_FIELDS = {"stage_ratio", "status_ratio", "toss_ratio", "stage_totals"}


def row_to_camel(row: sqlite3.Row) -> dict:
    out: dict = {}
    for k in row.keys():
        v = row[k]
        if k in JSON_FIELDS and isinstance(v, str):
            v = json.loads(v)
        out[SNAKE_TO_CAMEL_MAP.get(k, k)] = v
    return out


def call_power_report(conn: sqlite3.Connection, target_row: sqlite3.Row) -> dict:
    cur = conn.cursor()
    period = target_row["granularity"]
    period_start = target_row["time_start"]

    device_rows = cur.execute(
        "SELECT pe.*, d.name FROM power_energy pe JOIN device d ON d.id = pe.device_id "
        "WHERE pe.granularity = ? AND pe.time_start = ? AND pe.device_id IS NOT NULL",
        (period, period_start),
    ).fetchall()
    total = target_row["energy_wh"] or 1e-9
    by_device = [
        {"deviceId": r["device_id"], "name": r["name"], "energyWh": r["energy_wh"], "share": round(r["energy_wh"] / total, 4)}
        for r in device_rows
    ]

    peak_w = None
    peak_at = None
    if period == "24h":
        children_rows = cur.execute(
            "SELECT * FROM power_energy WHERE granularity='5m' AND device_id IS NULL AND time_start LIKE ?",
            (period_start + "%",),
        ).fetchall()
        if children_rows:
            peak_row = max(children_rows, key=lambda r: r["energy_wh"])
            peak_w = round(peak_row["energy_wh"] * 12, 1)  # 5분 Wh -> W
            peak_at = peak_row["time_start"]
        child_gran = "1h"
    else:
        child_gran = "24h"

    children = cur.execute(
        "SELECT * FROM power_energy WHERE granularity=? AND device_id IS NULL AND time_start >= ? AND time_start < ?",
        (
            child_gran,
            period_start,
            _period_end(period, period_start),
        ),
    ).fetchall()

    prev_start = _prev_period_start(period, period_start)
    prev_row = cur.execute(
        "SELECT energy_wh FROM power_energy WHERE granularity=? AND device_id IS NULL AND time_start=?",
        (period, prev_start),
    ).fetchone() if prev_start else None
    vs_prev_pct = None
    if prev_row and prev_row["energy_wh"]:
        vs_prev_pct = round((total - prev_row["energy_wh"]) / prev_row["energy_wh"] * 100, 1)

    metrics = {
        "energyWh": round(total, 2),
        "energyKwh": round(total / 1000, 3),
        "byDevice": by_device,
    }
    if peak_w is not None:
        metrics["peakW"] = peak_w
        metrics["peakAt"] = peak_at
    if vs_prev_pct is not None:
        metrics["vsPrevPct"] = vs_prev_pct
    if period in ("1w", "1mo"):
        n_days = 7 if period == "1w" else 30
        metrics["avgDailyKwh"] = round(total / 1000 / n_days, 3)

    body = {
        "deviceId": None,
        "period": period,
        "periodStart": period_start,
        "metrics": metrics,
        "target": row_to_camel(target_row),
        "children": [row_to_camel(r) for r in children],
        "embed": True,
    }
    result = agent_client.create_power_report(body)
    return {
        "energy_id": target_row["id"],
        "device_id": None,
        "period": period,
        "period_start": period_start,
        "metrics": metrics,
        "report_text": result["reportText"],
        "embedding": result["embedding"],
        "model": result.get("model"),
        "embedding_model": result.get("embeddingModel"),
    }


def _period_end(period: str, start: str) -> str:
    if period == "24h":
        d = datetime.strptime(start, "%Y-%m-%d") + timedelta(days=1)
        return timeutil.fmt_date(d.date())
    if period == "1w":
        d = datetime.strptime(start, "%Y-%m-%d") + timedelta(days=7)
        return timeutil.fmt_date(d.date())
    d = datetime.strptime(start, "%Y-%m-%d") + timedelta(days=30)
    return timeutil.fmt_date(d.date())


def _prev_period_start(period: str, start: str) -> str | None:
    d = datetime.strptime(start, "%Y-%m-%d")
    if period == "24h":
        prev = d - timedelta(days=1)
    elif period == "1w":
        prev = d - timedelta(days=7)
    else:
        return None
    if prev.date() < timeutil.MONTH_START:
        return None
    return timeutil.fmt_date(prev.date())


def gather_power_targets(conn: sqlite3.Connection) -> list[sqlite3.Row]:
    cur = conn.cursor()
    rows = cur.execute(
        "SELECT * FROM power_energy WHERE device_id IS NULL AND granularity IN ('24h','1w','1mo') ORDER BY granularity, time_start"
    ).fetchall()
    return rows


def call_sleep_daily(conn: sqlite3.Connection, night_date: str) -> dict:
    cur = conn.cursor()
    session = cur.execute("SELECT * FROM sleep_session WHERE night_date=?", (night_date,)).fetchone()
    if session is None:
        raise RuntimeError(f"no sleep_session for {night_date}")
    stats30m = cur.execute(
        "SELECT * FROM sleep_stat WHERE session_id=? AND granularity='30m' ORDER BY time_start", (session["id"],)
    ).fetchall()

    tib_s = session["time_in_bed_s"] or 0
    asleep_s = session["asleep_total_s"] or 0
    metrics = {
        "asleepTotalS": asleep_s,
        "timeInBedS": tib_s,
        "efficiency": session["efficiency"],
        "latencyS": None,
        "tossEvents": session["toss_events"],
        "snoreRatio": session["snore_ratio"],
    }
    body = {
        "userId": USER_ID,
        "period": "daily",
        "periodStart": night_date,
        "metrics": metrics,
        "sessions": [row_to_camel(session)],
        "stats30m": [row_to_camel(r) for r in stats30m],
        "embed": True,
    }
    result = agent_client.create_sleep_report(body)
    return {
        "period": "daily",
        "period_start": night_date,
        "session_id": session["id"],
        "metrics": metrics,
        "report_text": result["reportText"],
        "embedding": result["embedding"],
        "model": result.get("model"),
        "embedding_model": result.get("embeddingModel"),
    }


def call_sleep_weekly(conn: sqlite3.Connection, week_start: str) -> dict:
    cur = conn.cursor()
    week_end = timeutil.fmt_date(datetime.strptime(week_start, "%Y-%m-%d").date() + timedelta(days=7))
    sessions = cur.execute(
        "SELECT * FROM sleep_session WHERE night_date >= ? AND night_date < ? ORDER BY night_date",
        (week_start, week_end),
    ).fetchall()
    if not sessions:
        raise RuntimeError(f"no sessions for week {week_start}")
    session_ids = [s["id"] for s in sessions]
    placeholders = ",".join("?" * len(session_ids))
    stats30m = cur.execute(
        f"SELECT * FROM sleep_stat WHERE granularity='30m' AND session_id IN ({placeholders}) ORDER BY time_start",
        session_ids,
    ).fetchall()

    asleep_list = [s["asleep_total_s"] for s in sessions if s["asleep_total_s"] is not None]
    eff_list = [s["efficiency"] for s in sessions if s["efficiency"] is not None]
    metrics = {
        "nights": len(sessions),
        "avgAsleepS": round(sum(asleep_list) / len(asleep_list)) if asleep_list else None,
        "avgEfficiency": round(sum(eff_list) / len(eff_list), 3) if eff_list else None,
    }
    body = {
        "userId": USER_ID,
        "period": "weekly",
        "periodStart": week_start,
        "metrics": metrics,
        "sessions": [row_to_camel(s) for s in sessions],
        "stats30m": [row_to_camel(r) for r in stats30m],
        "embed": True,
    }
    result = agent_client.create_sleep_report(body)
    return {
        "period": "weekly",
        "period_start": week_start,
        "session_id": None,
        "metrics": metrics,
        "report_text": result["reportText"],
        "embedding": result["embedding"],
        "model": result.get("model"),
        "embedding_model": result.get("embeddingModel"),
    }


def _load_existing(path: Path) -> list[dict]:
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return []
    return []


def _save(path: Path, items: list[dict]) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(items, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(path)


def main() -> None:
    if not agent_client.health_check():
        print("에이전트 서버(:8501)가 응답하지 않습니다. 먼저 실행하세요.", file=sys.stderr)
        sys.exit(1)

    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row

    power_path = OUT_DIR / "power_reports.json"
    sleep_path = OUT_DIR / "sleep_reports.json"
    power_results: list[dict] = _load_existing(power_path)
    sleep_results: list[dict] = _load_existing(sleep_path)
    done_power_ids = {r["energy_id"] for r in power_results}
    done_sleep_keys = {(r["period"], r["period_start"]) for r in sleep_results}

    all_power_targets = gather_power_targets(conn)
    all_sleep_daily_dates = [row[0] for row in SCENARIO]
    all_sleep_weekly_starts = [
        timeutil.fmt_date(d) for d in timeutil.sliding_week_starts(timeutil.june_dates())
    ]

    power_targets = [r for r in all_power_targets if r["id"] not in done_power_ids]
    sleep_daily_dates = [d for d in all_sleep_daily_dates if ("daily", d) not in done_sleep_keys]
    sleep_weekly_starts = [w for w in all_sleep_weekly_starts if ("weekly", w) not in done_sleep_keys]

    print(
        f"이미 완료: power {len(done_power_ids)}건, sleep {len(sleep_results)}건 (재실행 시 스킵)\n"
        f"남은 작업 - power targets: {len(power_targets)} / sleep daily: {len(sleep_daily_dates)} / "
        f"sleep weekly: {len(sleep_weekly_starts)}",
        flush=True,
    )

    errors: list[str] = []

    def run_power(row):
        conn_local = sqlite3.connect(DB_PATH)
        conn_local.row_factory = sqlite3.Row
        try:
            return call_power_report(conn_local, row)
        finally:
            conn_local.close()

    def run_sleep_daily(date_str):
        conn_local = sqlite3.connect(DB_PATH)
        conn_local.row_factory = sqlite3.Row
        try:
            return call_sleep_daily(conn_local, date_str)
        finally:
            conn_local.close()

    def run_sleep_weekly(week_start):
        conn_local = sqlite3.connect(DB_PATH)
        conn_local.row_factory = sqlite3.Row
        try:
            return call_sleep_weekly(conn_local, week_start)
        finally:
            conn_local.close()

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as pool:
        futures = {}
        for row in power_targets:
            futures[pool.submit(run_power, row)] = f"power {row['granularity']} {row['time_start']}"
        for d in sleep_daily_dates:
            futures[pool.submit(run_sleep_daily, d)] = f"sleep daily {d}"
        for w in sleep_weekly_starts:
            futures[pool.submit(run_sleep_weekly, w)] = f"sleep weekly {w}"

        done = 0
        total = len(futures)
        for fut in as_completed(futures):
            label = futures[fut]
            done += 1
            try:
                result = fut.result()
                if "energy_id" in result:
                    power_results.append(result)
                    _save(power_path, power_results)
                else:
                    sleep_results.append(result)
                    _save(sleep_path, sleep_results)
                print(f"[{done}/{total}] OK  {label}", flush=True)
            except Exception as exc:  # noqa: BLE001
                print(f"[{done}/{total}] FAIL {label}: {exc}", flush=True)
                errors.append(f"{label}: {exc}")

    print("=== 02_call_agent_reports 완료 ===")
    print(f"power_reports: {len(power_results)} -> {power_path}")
    print(f"sleep_reports: {len(sleep_results)} -> {sleep_path}")
    if errors:
        print(f"errors: {len(errors)}")
        for e in errors:
            print(" -", e)

    conn.close()


if __name__ == "__main__":
    main()
