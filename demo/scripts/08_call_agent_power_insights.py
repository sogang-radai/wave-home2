#!/usr/bin/env python3
"""에이전트 /insight/v1/insights 로 surface=power 인사이트를 생성한다.

결과: demo/agent/power_insights.json
데모 백엔드(:8510)가 갱신된 power_energy/power_report 를 제공해야 한다
(에이전트 WAVEHOME_AGENT_INTERNAL_BASE_URL → agent-api).

실행:
    AGENT_BASE_URL=http://127.0.0.1:8512 python3 demo/scripts/08_call_agent_power_insights.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from _lib import agent_client

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_PATH = REPO_ROOT / "demo" / "agent" / "power_insights.json"
USER_ID = 1
SURFACE = "power"
# 시나리오 핵심일 + 앵커일(사용자 노출)
DATES = [
    "2026-06-10",
    "2026-06-14",
    "2026-06-18",
    "2026-06-25",
    "2026-06-26",
    "2026-06-29",
    "2026-06-30",
]
CREATED_AT = "2026-07-01 00:00:00"


def _load_existing() -> list[dict]:
    if OUT_PATH.exists():
        try:
            return json.loads(OUT_PATH.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return []
    return []


def _save(rows: list[dict]) -> None:
    tmp = OUT_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(OUT_PATH)


def _normalize_item(item: dict, date: str) -> dict:
    rule = item.get("ruleJson")
    sched = item.get("scheduleTaskJson")
    return {
        "user_id": USER_ID,
        "surface": item.get("surface") or SURFACE,
        "kind": item.get("kind") or "tip",
        "date": item.get("date") or date,
        "label": item.get("label") or "전력",
        "title": item["title"],
        "text": item["text"],
        "actionable": 1 if item.get("actionable") else 0,
        "action_type": item.get("actionType"),
        "approved": 0,
        "rule_json": json.dumps(rule, ensure_ascii=False) if rule else None,
        "schedule_task_json": json.dumps(sched, ensure_ascii=False) if sched else None,
        "embedding": item.get("embedding"),
        "created_at": CREATED_AT,
        "model": item.get("model"),
    }


def main() -> None:
    base = agent_client.DEFAULT_BASE_URL
    if not agent_client.health_check(base):
        print(f"에이전트 서버({base})가 응답하지 않습니다.", file=sys.stderr)
        sys.exit(1)
    print(f"agent={base} model={agent_client.DEFAULT_MODEL}", flush=True)

    rows = _load_existing()
    done_dates = {r["date"] for r in rows}
    pending = [d for d in DATES if d not in done_dates]
    print(f"이미 완료 날짜: {sorted(done_dates)} / 남은: {pending}", flush=True)

    errors: list[str] = []
    for i, date in enumerate(pending, 1):
        label = f"insight power {date}"
        try:
            result = agent_client.create_insights(
                {
                    "userId": USER_ID,
                    "surface": SURFACE,
                    "date": date,
                    "embed": True,
                    "model": agent_client.DEFAULT_MODEL,
                }
            )
            items = result.get("items") or []
            if not items and isinstance(result.get("insights"), list):
                items = result["insights"]
            if not items:
                raise RuntimeError(f"empty items: keys={list(result.keys())}")
            for item in items:
                rows.append(_normalize_item(item, date))
            _save(rows)
            kinds = sorted({(it.get("kind"), it.get("actionType")) for it in items})
            print(f"[{i}/{len(pending)}] OK  {label} items={len(items)} kinds={kinds}", flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"[{i}/{len(pending)}] FAIL {label}: {exc}", flush=True)
            errors.append(f"{label}: {exc}")

    print("=== 08_call_agent_power_insights 완료 ===")
    print(f"power_insights: {len(rows)} -> {OUT_PATH}")
    if errors:
        for e in errors:
            print(" -", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
