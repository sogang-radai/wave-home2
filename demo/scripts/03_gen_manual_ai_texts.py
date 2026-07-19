#!/usr/bin/env python3
"""에이전트 미지원 영역(insight, weekly_plan_report) + 템플릿 텍스트(1h 전력, 30m 수면 요약)를
manuel/템플릿으로 작성해 demo/ai_manual/*.json 에 저장한다(임베딩은 04에서 채움).
"""

from __future__ import annotations

import argparse
import json
import sqlite3
from pathlib import Path

from _lib import manual_texts

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "bin" / "data" / "demo.db"
OUT_DIR = REPO_ROOT / "demo" / "ai_manual"
AI_REPORTS_DIR = REPO_ROOT / "demo" / "agent"
OUT_DIR.mkdir(parents=True, exist_ok=True)

NOW = "2026-07-01 00:00:00"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--power-1h-only",
        action="store_true",
        help="power_report_1h.json 만 재생성(6/30 제외 템플릿)",
    )
    args = parser.parse_args()

    conn = sqlite3.connect(DB_PATH)
    power_1h = manual_texts.gen_power_1h_reports(conn, NOW)
    (OUT_DIR / "power_report_1h.json").write_text(
        json.dumps(power_1h, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"power_report(1h): {len(power_1h)}")

    if not args.power_1h_only:
        insights = manual_texts.build_insights(NOW)
        weekly_plans = manual_texts.gen_weekly_plan_reports(conn, NOW)
        sleep_30m = manual_texts.gen_sleep_30m_summaries(conn)
        rolling_weekly = manual_texts.merge_sleep_reports_rolling_weekly(
            conn, AI_REPORTS_DIR / "sleep_reports.json"
        )
        (OUT_DIR / "insight.json").write_text(json.dumps(insights, ensure_ascii=False, indent=2), encoding="utf-8")
        (OUT_DIR / "weekly_plan_report.json").write_text(
            json.dumps(weekly_plans, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        (OUT_DIR / "sleep_stat_30m_summary.json").write_text(
            json.dumps(sleep_30m, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(f"insight: {len(insights)}")
        print(f"weekly_plan_report: {len(weekly_plans)}")
        print(f"sleep_stat(30m) summaries: {len(sleep_30m)}")
        print(f"sleep_report weekly (rolling 7d): {rolling_weekly}건")

    conn.close()
    print("=== 03_gen_manual_ai_texts 완료 ===")


if __name__ == "__main__":
    main()
