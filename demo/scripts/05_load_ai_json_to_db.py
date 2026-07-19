#!/usr/bin/env python3
"""02(에이전트 실호출)·03/04(수동 작성+임베딩) 산출물을 최종 bin/data/demo.db 에 반영한다.

- demo/agent/power_reports.json  -> power_report(24h/1w/1mo) + vec_power_report
- demo/agent/sleep_reports.json  -> sleep_report(daily/weekly) + vec_sleep_report
- demo/ai_manual/power_report_1h.json -> power_report(1h) + vec_power_report
- demo/ai_manual/sleep_stat_30m_summary.json -> sleep_stat.summary_text(이미 있음) + vec_sleep_stat
- demo/ai_manual/insight.json         -> insight + vec_insight_{surface}
- demo/ai_manual/weekly_plan_report.json -> weekly_plan_report + vec_weekly_plan_report

여러 번 실행해도 안전하도록, 로드 전 대상 테이블의 관련 행을 지우고 다시 넣는다.
"""

from __future__ import annotations

import json
import sqlite3
from pathlib import Path

from _lib import ollama_client
from _lib.schema import INSIGHT_SURFACE_TO_VEC, ensure_runtime_schema

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "bin" / "data" / "demo.db"
AI_REPORTS_DIR = REPO_ROOT / "demo" / "agent"
AI_MANUAL_DIR = REPO_ROOT / "demo" / "ai_manual"


def load_json(path: Path) -> list[dict]:
    if not path.exists():
        print(f"  (없음, 스킵) {path}")
        return []
    return json.loads(path.read_text(encoding="utf-8"))


def load_power_reports(conn: sqlite3.Connection, vec_ready: bool) -> int:
    cur = conn.cursor()
    cur.execute("DELETE FROM vec_power_report") if vec_ready else None
    cur.execute("DELETE FROM power_report")

    rows = load_json(AI_REPORTS_DIR / "power_reports.json")
    rows += load_json(AI_MANUAL_DIR / "power_report_1h.json")

    n = 0
    for r in rows:
        cur.execute(
            "INSERT INTO power_report (energy_id, device_id, period, period_start, metrics, report_text, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                r["energy_id"], r.get("device_id"), r["period"], r["period_start"],
                json.dumps(r["metrics"], ensure_ascii=False), r["report_text"],
                r.get("created_at", "2026-07-01 00:00:00"),
            ),
        )
        report_id = cur.lastrowid
        if vec_ready and r.get("embedding"):
            cur.execute(
                "INSERT INTO vec_power_report (report_id, embedding) VALUES (?, ?)",
                (report_id, ollama_client.to_vec_blob(r["embedding"])),
            )
        n += 1
    conn.commit()
    return n


def load_sleep_reports(conn: sqlite3.Connection, vec_ready: bool) -> int:
    cur = conn.cursor()
    if vec_ready:
        cur.execute("DELETE FROM vec_sleep_report")
    cur.execute("DELETE FROM sleep_report")

    rows = load_json(AI_REPORTS_DIR / "sleep_reports.json")
    n = 0
    for r in rows:
        cur.execute(
            "INSERT INTO sleep_report (user_id, period, period_start, session_id, metrics, report_text) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (1, r["period"], r["period_start"], r.get("session_id"),
             json.dumps(r["metrics"], ensure_ascii=False), r["report_text"]),
        )
        report_id = cur.lastrowid
        if vec_ready and r.get("embedding"):
            cur.execute(
                "INSERT INTO vec_sleep_report (report_id, embedding) VALUES (?, ?)",
                (report_id, ollama_client.to_vec_blob(r["embedding"])),
            )
        n += 1
    conn.commit()
    return n


def load_sleep_30m_summaries(conn: sqlite3.Connection, vec_ready: bool) -> int:
    cur = conn.cursor()
    if vec_ready:
        cur.execute("DELETE FROM vec_sleep_stat")

    rows = load_json(AI_MANUAL_DIR / "sleep_stat_30m_summary.json")
    n = 0
    for r in rows:
        cur.execute("UPDATE sleep_stat SET summary_text = ? WHERE id = ?", (r["summary_text"], r["stat_id"]))
        if vec_ready and r.get("embedding"):
            cur.execute(
                "INSERT INTO vec_sleep_stat (stat_id, embedding) VALUES (?, ?)",
                (r["stat_id"], ollama_client.to_vec_blob(r["embedding"])),
            )
        n += 1
    conn.commit()
    return n


def load_insights(conn: sqlite3.Connection, vec_ready: bool) -> int:
    cur = conn.cursor()
    if vec_ready:
        for vec_table in set(INSIGHT_SURFACE_TO_VEC.values()):
            cur.execute(f"DELETE FROM {vec_table}")
    cur.execute("DELETE FROM insight")

    rows = load_json(AI_MANUAL_DIR / "insight.json")
    n = 0
    for r in rows:
        cur.execute(
            "INSERT INTO insight (user_id, surface, kind, date, label, title, text, actionable, action_type, "
            "approved, rule_json, schedule_task_json, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                r["user_id"], r["surface"], r["kind"], r["date"], r.get("label"), r["title"], r["text"],
                r["actionable"], r.get("action_type"), r["approved"], r.get("rule_json"),
                r.get("schedule_task_json"), r["created_at"],
            ),
        )
        insight_id = cur.lastrowid
        if vec_ready and r.get("embedding"):
            vec_table = INSIGHT_SURFACE_TO_VEC[r["surface"]]
            cur.execute(
                f"INSERT INTO {vec_table} (insight_id, embedding) VALUES (?, ?)",
                (insight_id, ollama_client.to_vec_blob(r["embedding"])),
            )
        n += 1
    conn.commit()
    return n


def load_weekly_plan_reports(conn: sqlite3.Connection, vec_ready: bool) -> int:
    cur = conn.cursor()
    if vec_ready:
        cur.execute("DELETE FROM vec_weekly_plan_report")
    cur.execute("DELETE FROM weekly_plan_report")

    rows = load_json(AI_MANUAL_DIR / "weekly_plan_report.json")
    n = 0
    for r in rows:
        cur.execute(
            "INSERT INTO weekly_plan_report (user_id, period_start, headline, report_text, created_at) "
            "VALUES (?, ?, ?, ?, ?)",
            (r["user_id"], r["period_start"], r.get("headline"), r["report_text"], r["created_at"]),
        )
        report_id = cur.lastrowid
        if vec_ready and r.get("embedding"):
            cur.execute(
                "INSERT INTO vec_weekly_plan_report (report_id, embedding) VALUES (?, ?)",
                (report_id, ollama_client.to_vec_blob(r["embedding"])),
            )
        n += 1
    conn.commit()
    return n


def ensure_calendar_monthly_power_reports(conn: sqlite3.Connection) -> int:
    """월간(1mo) 리포트가 1~6월 각각의 달 1일 period_start 를 갖도록 정리한다."""
    cur = conn.cursor()
    june = cur.execute(
        "SELECT energy_id, metrics, report_text, created_at FROM power_report "
        "WHERE period='1mo' AND device_id IS NULL AND period_start LIKE '2026-06%' "
        "ORDER BY period_start DESC LIMIT 1"
    ).fetchone()
    if not june:
        return 0

    energy_id, june_metrics, june_text, created_at = june
    cur.execute("DELETE FROM power_report WHERE period='1mo' AND device_id IS NULL")
    n = 0
    for month in range(1, 7):
        period_start = f"2026-{month:02d}-01"
        if month == 6:
            cur.execute(
                "INSERT INTO power_report (energy_id, device_id, period, period_start, metrics, report_text, created_at) "
                "VALUES (?, NULL, '1mo', ?, ?, ?, ?)",
                (energy_id, period_start, june_metrics, june_text, created_at),
            )
        else:
            cur.execute(
                "INSERT INTO power_report (energy_id, device_id, period, period_start, metrics, report_text, created_at) "
                "VALUES (?, NULL, '1mo', ?, '{}', '리포트 준비 중입니다.', ?)",
                (energy_id, period_start, created_at),
            )
        n += 1
    conn.commit()
    return n


def main() -> None:
    conn = sqlite3.connect(DB_PATH)
    vec_ready = ensure_runtime_schema(conn, with_vec=True)
    if not vec_ready:
        print(
            "경고: sqlite-vec 확장을 로드할 수 없어 vec_* 테이블 반영을 건너뜁니다.\n"
            "      uv run --with sqlite-vec demo/scripts/00_ensure_schema.py 를 먼저 실행하세요."
        )

    print(f"power_report: {load_power_reports(conn, vec_ready)}건")
    print(f"power_report(1mo calendar): {ensure_calendar_monthly_power_reports(conn)}건")
    print(f"sleep_report: {load_sleep_reports(conn, vec_ready)}건")
    print(f"sleep_stat(30m summary): {load_sleep_30m_summaries(conn, vec_ready)}건")
    print(f"insight: {load_insights(conn, vec_ready)}건")
    print(f"weekly_plan_report: {load_weekly_plan_reports(conn, vec_ready)}건")

    conn.close()
    print("=== 05_load_ai_json_to_db 완료 ===")


if __name__ == "__main__":
    main()
