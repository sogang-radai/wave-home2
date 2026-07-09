#!/usr/bin/env python3
"""에이전트 미지원 영역(insight, weekly_plan_report) + 템플릿 텍스트(1h 전력, 30m 수면 요약)를
manuel/템플릿으로 작성해 mock/ai_manual/*.json 에 저장한다(임베딩은 04에서 채움).
"""

from __future__ import annotations

import json
import sqlite3
from pathlib import Path

from lib import manual_texts

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "mock" / "data" / "mock.db"
OUT_DIR = REPO_ROOT / "mock" / "ai_manual"
OUT_DIR.mkdir(parents=True, exist_ok=True)

NOW = "2026-07-01 00:00:00"


def main() -> None:
    conn = sqlite3.connect(DB_PATH)

    insights = manual_texts.build_insights(NOW)
    weekly_plans = manual_texts.gen_weekly_plan_reports(conn, NOW)
    power_1h = manual_texts.gen_power_1h_reports(conn, NOW)
    sleep_30m = manual_texts.gen_sleep_30m_summaries(conn)

    (OUT_DIR / "insight.json").write_text(json.dumps(insights, ensure_ascii=False, indent=2), encoding="utf-8")
    (OUT_DIR / "weekly_plan_report.json").write_text(json.dumps(weekly_plans, ensure_ascii=False, indent=2), encoding="utf-8")
    (OUT_DIR / "power_report_1h.json").write_text(json.dumps(power_1h, ensure_ascii=False, indent=2), encoding="utf-8")
    (OUT_DIR / "sleep_stat_30m_summary.json").write_text(json.dumps(sleep_30m, ensure_ascii=False, indent=2), encoding="utf-8")

    print("=== 03_gen_manual_ai_texts 완료 ===")
    print(f"insight: {len(insights)}")
    print(f"weekly_plan_report: {len(weekly_plans)}")
    print(f"power_report(1h): {len(power_1h)}")
    print(f"sleep_stat(30m) summaries: {len(sleep_30m)}")

    conn.close()


if __name__ == "__main__":
    main()
