#!/usr/bin/env python3
"""02(에이전트 실호출)·03/04(수동 작성+임베딩) 산출물을 최종 mock/data/mock.db 에 반영한다.

- mock/ai_reports/power_reports.json  -> power_report(24h/1w/1mo) + vec_power_report
- mock/ai_reports/sleep_reports.json  -> sleep_report(daily/weekly) + vec_sleep_report
- mock/ai_manual/power_report_1h.json -> power_report(1h) + vec_power_report
- mock/ai_manual/sleep_stat_30m_summary.json -> sleep_stat.summary_text(이미 있음) + vec_sleep_stat
- mock/ai_manual/insight.json         -> insight + vec_insight_{surface}
- mock/ai_manual/weekly_plan_report.json -> weekly_plan_report + vec_weekly_plan_report

여러 번 실행해도 안전하도록, 로드 전 대상 테이블의 관련 행을 지우고 다시 넣는다.
"""

from __future__ import annotations

import json
import sqlite3
from pathlib import Path

from lib import ollama_client
from lib.schema import INSIGHT_SURFACE_TO_VEC, try_load_vec_extension

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "mock" / "data" / "mock.db"
AI_REPORTS_DIR = REPO_ROOT / "mock" / "ai_reports"
AI_MANUAL_DIR = REPO_ROOT / "mock" / "ai_manual"


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


def main() -> None:
    conn = sqlite3.connect(DB_PATH)
    vec_ready = try_load_vec_extension(conn)
    if not vec_ready:
        print("경고: sqlite-vec 확장을 로드할 수 없어 vec_* 테이블 반영을 건너뜁니다.")

    print(f"power_report: {load_power_reports(conn, vec_ready)}건")
    print(f"sleep_report: {load_sleep_reports(conn, vec_ready)}건")
    print(f"sleep_stat(30m summary): {load_sleep_30m_summaries(conn, vec_ready)}건")
    print(f"insight: {load_insights(conn, vec_ready)}건")
    print(f"weekly_plan_report: {load_weekly_plan_reports(conn, vec_ready)}건")

    conn.close()
    print("=== 05_load_ai_json_to_db 완료 ===")


if __name__ == "__main__":
    main()
