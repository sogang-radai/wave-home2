"""에이전트가 지원하지 않는 영역(insight, weekly_plan_report)의 수동 텍스트 + 1h 전력 템플릿.

Phase 1~3에서 만든 실제 데이터(사건 날짜, 점수, kWh 등)를 그대로 인용해 문장을 만든다
(완전 창작이 아니라 데이터 기반 서술이 되도록).
"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timedelta

from .sleep_scenario import SCENARIO
from .timeutil import MONTH_START, june_dates

SCENARIO_BY_DATE = {row[0]: row for row in SCENARIO}


# ---------------------------------------------------------------------------
# insight (5 surface, 수동 작성)
# ---------------------------------------------------------------------------
def build_insights(now: str) -> list[dict]:
    def item(user_id, surface, kind, date, title, text, label=None, actionable=0, action_type=None,
              rule_json=None, schedule_task_json=None):
        return {
            "user_id": user_id, "surface": surface, "kind": kind, "date": date, "label": label,
            "title": title, "text": text, "actionable": actionable, "action_type": action_type,
            "approved": 0,
            "rule_json": json.dumps(rule_json, ensure_ascii=False) if rule_json else None,
            "schedule_task_json": json.dumps(schedule_task_json, ensure_ascii=False) if schedule_task_json else None,
            "created_at": now,
        }

    items: list[dict] = []

    # dashboard_banner
    items += [
        item(1, "dashboard_banner", "banner", "2026-06-03", "독서 루틴이 자리잡고 있어요",
             "최근 며칠 취침 전 독서를 챙기시면서 입면까지 걸리는 시간이 조금씩 줄고 있어요.", label="수면"),
        item(1, "dashboard_banner", "tip", "2026-06-10", "오늘은 재택근무라 낮 활동이 적었어요",
             "낮에 집에 계신 시간이 길었던 날은 밤 입면이 늦어지는 편이에요. 짧은 산책은 어떨까요?", label="생활"),
        item(2, "dashboard_banner", "banner", "2026-06-13", "이번 주 헬스장 3회 모두 완료했어요",
             "월·수·금 헬스장 일정을 모두 지키셨어요. 좋은 페이스예요!", label="운동"),
        item(1, "dashboard_banner", "action", "2026-06-18", "오늘 밤 침실이 더울 것 같아요",
             "폭염 예보가 있어요. 취침 전 에어컨을 평소보다 일찍 켜두는 걸 추천해요.",
             label="전력·수면", actionable=1, action_type="automation_rule",
             rule_json={"name": "폭염 대비 조기 냉방", "trigger": {"type": "manual"},
                        "actions": [{"deviceId": "4a2d9c7f1e60b358", "name": "on", "params": {"temp": 24}}]}),
        item(1, "dashboard_banner", "tip", "2026-06-19", "어젯밤 더위 때문에 잠이 조금 얕았어요",
             "에어컨을 강하게 가동해두시면 오늘 밤은 더 편히 잠드실 수 있을 거예요.", label="수면"),
        item(2, "dashboard_banner", "tip", "2026-06-20", "요즘 저녁 식단 기록이 꾸준해요",
             "이번 주 저녁 식단을 꼬박꼬박 기록하고 계세요. 단백질 섭취가 특히 좋아졌어요.", label="식단"),
        item(1, "dashboard_banner", "banner", "2026-06-25", "오늘 저녁엔 모임이 있으시네요",
             "저녁 모임 일정이 있어요. 늦게 주무시더라도 다음날은 무리하지 않는 게 좋아요.", label="일정"),
        item(1, "dashboard_banner", "banner", "2026-06-30", "이번 달 수면이 눈에 띄게 좋아졌어요",
             "월초 대비 수면 점수가 꾸준히 올랐어요. 독서·스트레칭 루틴 효과가 보이네요.", label="수면"),
    ]

    # sleep_report
    items += [
        item(1, "sleep_report", "tip", "2026-06-02", "입면까지 시간이 조금 걸렸어요",
             "어젯밤은 잠들기까지 평소보다 오래 걸렸어요. 얕은 수면 비중도 높았습니다.", label="수면"),
        item(1, "sleep_report", "goal", "2026-06-08", "이번 주 취침 시각이 안정적이에요",
             "최근 며칠 23시 전후로 꾸준히 취침하고 계세요. 이 리듬을 유지해보세요.", label="수면"),
        item(1, "sleep_report", "action", "2026-06-18", "깊은 수면이 크게 줄었어요",
             "어젯밤 깊은 수면 비중이 6%로 평소보다 크게 낮았어요. 침실 온도가 26도를 넘었던 영향이 커요.",
             label="수면", actionable=1, action_type="schedule_task",
             schedule_task_json={"title": "취침 전 침실 미리 냉방", "category": "sleep", "scheduleKind": "weekly",
                                  "dayOfWeek": "sun", "startMinute": 1320, "endMinute": 1350}),
        item(1, "sleep_report", "tip", "2026-06-25", "수면 시간이 많이 짧았어요",
             "어젯밤은 5시간 36분만 주무셨어요. 오늘은 평소보다 일찍 쉬시는 걸 추천해요.", label="수면"),
        item(1, "sleep_report", "banner", "2026-06-29", "한 달 중 가장 좋은 수면이었어요",
             "점수 90점, 깊은 수면 20% — 이번 달 최고 기록이에요.", label="수면"),
    ]

    # power
    items += [
        item(1, "power", "tip", "2026-06-10", "낮 시간 전력 사용이 평소와 달랐어요",
             "재택근무로 낮에도 전력을 사용하셨어요. 평소보다 사용 패턴이 넓게 퍼졌습니다.", label="전력"),
        item(1, "power", "action", "2026-06-18", "에어컨 사용량이 크게 늘었어요",
             "폭염으로 침실 에어컨 사용량이 급증했어요. 사용 시간대를 조정하면 절약할 수 있어요.",
             label="전력", actionable=1, action_type="reservation",
             rule_json={"name": "야간 냉방 예약", "deviceId": "4a2d9c7f1e60b358", "onMinute": 1380, "offMinute": 420}),
        item(1, "power", "tip", "2026-06-25", "저녁 시간 조리 전력이 늘었어요",
             "모임 준비로 인덕션 사용이 평소보다 길었어요. 저녁 시간대 전력 사용이 집중됐습니다.", label="전력"),
        item(1, "power", "goal", "2026-06-30", "이번 달 전력 사용, 에어컨이 대부분이었어요",
             "한 달간 사용량의 대부분이 침실 에어컨이었어요. 다음 달엔 온도 설정을 1도만 올려보는 건 어떨까요?",
             label="전력"),
    ]

    # weekly_plan (요약 배너 — weekly_plan_report 와 별개로 짧은 하이라이트)
    for user_id, week_start, headline in (
        (1, "2026-06-01", "리듬을 다시 잡아가는 한 주예요"),
        (1, "2026-06-08", "수면 루틴이 안정적으로 자리잡았어요"),
        (1, "2026-06-18", "더위 때문에 흔들렸지만 잘 회복했어요"),
        (1, "2026-06-25", "모임 다음날, 무리하지 않는 게 좋아요"),
        (2, "2026-06-08", "헬스장 루틴을 꾸준히 지키고 있어요"),
    ):
        items.append(item(user_id, "weekly_plan", "banner", week_start, headline,
                          f"{headline} 자세한 내용은 주간 계획 리포트를 확인해보세요.", label="주간계획"))

    # posture_report — 데이터 미확정이라 일반 안내만 1건
    items.append(item(1, "posture_report", "tip", "2026-06-01", "자세 분석은 준비 중이에요",
                       "책상 레이더 데이터를 기반으로 한 자세 분석 기능은 곧 제공될 예정이에요.", label="자세"))

    return items


# ---------------------------------------------------------------------------
# weekly_plan_report (30 슬라이딩 x 2 사용자)
# ---------------------------------------------------------------------------
def _window_dates(end_date_str: str) -> list[str]:
    end = datetime.strptime(end_date_str, "%Y-%m-%d").date()
    start = max(MONTH_START, end - timedelta(days=6))
    n = (end - start).days + 1
    return [(start + timedelta(days=i)).strftime("%Y-%m-%d") for i in range(n)]


def _notes_in_window(dates: list[str]) -> list[str]:
    notes = []
    for d in dates:
        row = SCENARIO_BY_DATE.get(d)
        if row and row[7]:
            notes.append(f"{d[5:]}: {row[7]}")
    return notes


def gen_weekly_plan_reports(conn: sqlite3.Connection, now: str) -> list[dict]:
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    out: list[dict] = []
    prev_avg_score: dict[int, float] = {}
    prev_avg_kwh: float | None = None

    for d in june_dates():
        date_str = d.strftime("%Y-%m-%d")
        window = _window_dates(date_str)
        start, end = window[0], window[-1]
        n_days = len(window)

        sessions = cur.execute(
            "SELECT * FROM sleep_session WHERE night_date >= ? AND night_date <= ?", (start, end)
        ).fetchall()
        avg_eff = None
        if sessions:
            effs = [s["efficiency"] for s in sessions if s["efficiency"] is not None]
            avg_eff = round(sum(effs) / len(effs) * 100, 1) if effs else None
        scores = [SCENARIO_BY_DATE[s["night_date"]][4] for s in sessions if s["night_date"] in SCENARIO_BY_DATE]
        avg_score = round(sum(scores) / len(scores), 1) if scores else None

        power_rows = cur.execute(
            "SELECT energy_wh FROM power_energy WHERE device_id IS NULL AND granularity='24h' "
            "AND time_start >= ? AND time_start <= ?", (start, end)
        ).fetchall()
        avg_kwh = round(sum(r["energy_wh"] for r in power_rows) / len(power_rows) / 1000, 2) if power_rows else None

        notes = _notes_in_window(window)
        note_str = " / ".join(notes) if notes else "특별한 이벤트는 없었어요"

        for user_id in (1, 2):
            if user_id == 1 and avg_score is not None:
                trend = ""
                if user_id in prev_avg_score and prev_avg_score[user_id] is not None:
                    diff = avg_score - prev_avg_score[user_id]
                    trend = "개선되고 있어요" if diff > 1 else ("주의가 필요해요" if diff < -3 else "비슷한 수준을 유지하고 있어요")
                headline = f"최근 {n_days}일 평균 수면 점수 {avg_score}점"
                text = (
                    f"{start}~{end}({n_days}일) 동안 평균 수면 점수는 {avg_score}점, 평균 수면 효율은 "
                    f"{avg_eff if avg_eff is not None else '알수없음'}% 였어요. "
                    f"지난 기간과 비교하면 {trend if trend else '데이터가 더 필요해요'}. "
                    f"이 기간 특이사항: {note_str}. "
                    f"평균 가정 전력 사용량은 하루 {avg_kwh if avg_kwh is not None else '알수없음'}kWh 였습니다. "
                    "아침 스트레칭과 취침 전 독서 루틴을 계속 유지해보세요."
                )
                prev_avg_score[user_id] = avg_score
            else:
                trend = ""
                if prev_avg_kwh is not None and avg_kwh is not None:
                    diff = avg_kwh - prev_avg_kwh
                    trend = "늘었어요" if diff > 0.3 else ("줄었어요" if diff < -0.3 else "비슷했어요")
                headline = f"최근 {n_days}일 평균 가정 전력 {avg_kwh if avg_kwh is not None else '?'}kWh/일"
                text = (
                    f"{start}~{end}({n_days}일) 동안 가정 평균 전력 사용량은 하루 {avg_kwh if avg_kwh is not None else '알수없음'}kWh"
                    f"{'로 지난 기간보다 ' + trend if trend else ''}. "
                    f"이 기간 특이사항: {note_str}. "
                    "헬스장·식단 루틴을 꾸준히 지키고 계세요. 저녁 시간 인덕션 사용에 주의해보세요."
                )

            out.append({
                "user_id": user_id,
                "period_start": date_str,
                "headline": headline,
                "report_text": text,
                "created_at": now,
            })
        prev_avg_kwh = avg_kwh

    return out


# ---------------------------------------------------------------------------
# 1h 전력 리포트(템플릿, device_id=NULL 만)
# ---------------------------------------------------------------------------
def gen_power_1h_reports(conn: sqlite3.Connection, now: str) -> list[dict]:
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    rows = cur.execute(
        "SELECT * FROM power_energy WHERE device_id IS NULL AND granularity='1h' ORDER BY time_start"
    ).fetchall()

    out = []
    for r in rows:
        hour = int(r["time_start"][11:13])
        kwh = round(r["energy_wh"] / 1000, 3)
        if kwh < 0.05:
            desc = "대기전력 수준의 사용량"
        elif kwh < 0.3:
            desc = "가벼운 사용량"
        elif kwh < 0.8:
            desc = "보통 수준의 사용량"
        else:
            desc = "높은 사용량"
        text = f"{r['time_start'][:10]} {hour:02d}시대 전력 사용량은 {kwh}kWh 로 {desc}이었습니다."
        out.append({
            "energy_id": r["id"],
            "device_id": None,
            "period": "1h",
            "period_start": r["time_start"],
            "metrics": {"energyWh": r["energy_wh"], "energyKwh": kwh},
            "report_text": text,
            "created_at": now,
        })
    return out


def gen_sleep_30m_summaries(conn: sqlite3.Connection) -> list[dict]:
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    rows = cur.execute(
        "SELECT id, summary_text FROM sleep_stat WHERE granularity='30m' AND summary_text IS NOT NULL ORDER BY id"
    ).fetchall()
    return [{"stat_id": r["id"], "summary_text": r["summary_text"]} for r in rows]
