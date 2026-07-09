"""제스처/자동화/일정/알람/알림/채팅 등 AI 불필요 서사 데이터 생성기.

김건강(수면·독서·명상 루틴)과 박헬스(헬스장·식단 루틴)에게 자연스러운 생활 서사를
부여해 6월 한 달 분량을 만든다.
"""

from __future__ import annotations

import json
import random

from . import timeutil

# ---------------------------------------------------------------------------
# 제스처 (침실 책상 레이더 -> TV / 침실 조명)
# ---------------------------------------------------------------------------
GESTURES = [
    (1, "swipe_right", "tv", "volume_up"),
    (2, "swipe_left", "tv", "volume_down"),
    (3, "palm_push", "light", "toggle"),
    (4, "circle_cw", "light", "brightness_up"),
    (5, "circle_ccw", "light", "brightness_down"),
    (6, "double_tap", "tv", "power_toggle"),
]


def gen_gesture_log(desk_radar_pk: int, tv_pk: int, light_pk: int, rng: random.Random) -> list[tuple]:
    """gesture_set_id, class_id, timestamp, gesture_name, radar_id, device_id, action, confidence"""
    rows: list[tuple] = []
    for d in timeutil.june_dates():
        weekend = timeutil.is_weekend(d)
        window_start_h = 13 if weekend else 19
        n = rng.randint(10, 20)
        used_minutes: set[int] = set()
        for _ in range(n):
            # window_start_h ~ 23:59 사이 임의 시각(겹침 방지 위해 분 단위 재시도).
            for _try in range(5):
                minute_of_window = rng.randint(0, (24 - window_start_h) * 60 - 1)
                if minute_of_window not in used_minutes:
                    used_minutes.add(minute_of_window)
                    break
            hh = window_start_h + minute_of_window // 60
            mm = minute_of_window % 60
            ss = rng.randint(0, 59)
            ts = f"{timeutil.fmt_date(d)} {hh:02d}:{mm:02d}:{ss:02d}"

            class_id, name, target, action = rng.choice(GESTURES)
            device_pk = tv_pk if target == "tv" else light_pk
            confidence = round(rng.uniform(0.72, 0.99), 3)
            rows.append((1, class_id, ts, name, desk_radar_pk, device_pk, action, confidence))
    rows.sort(key=lambda r: r[2])
    return rows


# ---------------------------------------------------------------------------
# 홈 자동화 룰
# ---------------------------------------------------------------------------
def gen_automation_rules(hex_ids: dict[str, str], now: str) -> list[tuple]:
    def actions(device_hex: str, name: str, params: dict | None = None) -> str:
        return json.dumps(
            [{"deviceId": device_hex, "name": name, "params": params or {}, "execMode": "once", "repeatIntervalMs": 0}],
            ensure_ascii=False,
        )

    rows = [
        (
            1, "rule_schedule_bedroom_light_off", "취침 시간 자동 소등", 1, 0,
            None, json.dumps({"cron": "0 23 * * *"}, ensure_ascii=False),
            actions(hex_ids["bedroom_light"], "off"),
            now, now,
        ),
        (
            1, "rule_trigger_aircon_off_away", "외출 시 침실 에어컨 자동 정지", 1, 300000,
            json.dumps({"deviceQuery": "presence", "condition": "absent"}, ensure_ascii=False), None,
            actions(hex_ids["aircon_plug"], "off"),
            now, now,
        ),
        (
            1, "rule_schedule_wake_light_ramp", "기상 조명 서서히 밝히기", 1, 0,
            None, json.dumps({"cron": "30 6 * * 1-5"}, ensure_ascii=False),
            actions(hex_ids["bedroom_light"], "ramp_on", {"durationMs": 1800000}),
            now, now,
        ),
        (
            2, "rule_schedule_induction_safety_off", "인덕션 안전 타이머", 1, 0,
            json.dumps({"afterDeviceOn": hex_ids["induction_plug"]}, ensure_ascii=False),
            json.dumps({"relativeMinutes": 30}, ensure_ascii=False),
            actions(hex_ids["induction_plug"], "off"),
            now, now,
        ),
    ]
    return rows


def gen_home_events(
    pks: dict[str, int],
    gesture_rows: list[tuple],
    rng: random.Random,
) -> list[tuple]:
    """user_id, type, occurred_at, device_id, device_name, message, triggered_by, detail_json"""
    rows: list[tuple] = []
    for d in timeutil.june_dates():
        date_s = timeutil.fmt_date(d)
        weekday = not timeutil.is_weekend(d)

        rows.append((
            1, "schedule", f"{date_s} 23:00:03", pks["bedroom_light"], "침실 조명",
            "취침 시간 자동 소등 규칙이 실행되어 침실 조명을 껐습니다.", "rule:1", None,
        ))
        if weekday:
            rows.append((
                1, "schedule", f"{date_s} 06:30:02", pks["bedroom_light"], "침실 조명",
                "기상 조명 서서히 밝히기 규칙이 실행되었습니다.", "rule:3", None,
            ))

        if rng.random() < 0.08:
            plug_name, plug_pk = rng.choice([
                ("거실 선풍기", pks["fan_plug"]), ("침실 컴퓨터", pks["pc_plug"]),
                ("침실 에어컨", pks["aircon_plug"]), ("부엌 인덕션", pks["induction_plug"]),
            ])
            hh = rng.randint(0, 23)
            rows.append((
                1, "connection", f"{date_s} {hh:02d}:{rng.randint(0,59):02d}:00", plug_pk, plug_name,
                f"{plug_name} 연결이 잠시 끊겼다가 복구되었습니다.", "device:reconnect", None,
            ))

    same_day_gestures: dict[str, list[tuple]] = {}
    for row in gesture_rows:
        ts = row[2]
        same_day_gestures.setdefault(ts[:10], []).append(row)
    for date_s, day_gestures in same_day_gestures.items():
        sample = rng.sample(day_gestures, k=min(4, len(day_gestures)))
        for _gs, _cid, ts, name, radar_id, device_id, action, confidence in sample:
            device_name = "침실 TV" if device_id == pks["tv"] else "침실 조명"
            rows.append((
                1, "gesture", ts, device_id, device_name,
                f"제스처({name})로 {device_name} {action} 실행", "gesture", json.dumps({"confidence": confidence}),
            ))

    rows.sort(key=lambda r: r[2])
    return rows


# ---------------------------------------------------------------------------
# 일정 (schedule_task)
# ---------------------------------------------------------------------------
def gen_schedule_tasks(created_at: str) -> list[tuple]:
    """user_id, title, created_at, created_by, category, schedule_kind, day_of_week, event_date,
    start_minute, end_minute, done, source_insight_id"""
    rows: list[tuple] = []

    def weekly(user_id, title, category, days, start_h, start_m, end_h, end_m, done=0):
        for dow in days:
            rows.append((
                user_id, title, created_at, "user", category, "weekly", dow, None,
                start_h * 60 + start_m, end_h * 60 + end_m, done, None,
            ))

    def once(user_id, title, category, event_date, dow, start_h, start_m, end_h, end_m, done=0):
        rows.append((
            user_id, title, created_at, "user", category, "once", dow, event_date,
            start_h * 60 + start_m, end_h * 60 + end_m, done, None,
        ))

    weekdays = ["mon", "tue", "wed", "thu", "fri"]
    weekend = ["sat", "sun"]
    all_days = weekdays + weekend

    # 김건강 - 수면/멘탈 루틴
    weekly(1, "아침 스트레칭", "posture", weekdays, 7, 0, 7, 15)
    weekly(1, "취침 전 독서", "mental", all_days, 22, 30, 23, 0)
    weekly(1, "주말 명상", "mental", weekend, 9, 0, 9, 20)
    once(1, "치과 정기검진", "life", "2026-06-16", "tue", 14, 0, 15, 0)
    once(1, "여름 옷 정리", "life", "2026-06-20", "sat", 10, 0, 11, 30)

    # 박헬스 - 헬스/식단 루틴
    weekly(2, "헬스장 운동", "fitness", ["mon", "wed", "fri"], 19, 0, 20, 30)
    weekly(2, "저녁 식단 관리", "diet", weekdays, 18, 0, 18, 30)
    weekly(2, "주말 등산", "fitness", ["sat"], 8, 0, 11, 0)
    once(2, "헬스 PT 상담", "fitness", "2026-06-05", "fri", 20, 30, 21, 0)

    return rows


# ---------------------------------------------------------------------------
# 알람
# ---------------------------------------------------------------------------
def gen_alarms(pks: dict[str, int], created_at: str) -> list[tuple]:
    """user_id, name, time_minute, days_of_week(json), smart_wake, radar_device_id, device_id,
    method(json), enabled, created_at, updated_at"""

    def method(sound_id: str, volume: int = 65) -> str:
        return json.dumps({"type": "sound", "soundId": sound_id, "volume": volume}, ensure_ascii=False)

    rows = [
        (
            1, "평일 기상", 7 * 60, json.dumps(["mon", "tue", "wed", "thu", "fri"]), 1,
            pks["bed_radar"], pks["bedroom_light"], method("morning-breeze"), 1, created_at, created_at,
        ),
        (
            1, "주말 늦은 기상", 8 * 60 + 30, json.dumps(["sat", "sun"]), 1,
            pks["bed_radar"], pks["bedroom_light"], method("gentle-piano"), 1, created_at, created_at,
        ),
        (
            1, "여행 조기 기상", 5 * 60 + 30, json.dumps([]), 0,
            None, None, method("alarm-classic", 80), 0, created_at, created_at,
        ),
        (
            2, "헬스장 가는 날 기상", 6 * 60 + 30, json.dumps(["mon", "wed", "fri"]), 0,
            None, None, method("energetic-beat", 75), 1, created_at, created_at,
        ),
        (
            2, "주말 아침 알람", 8 * 60, json.dumps(["sat"]), 0,
            None, None, method("gentle-piano"), 1, created_at, created_at,
        ),
    ]
    return rows


# ---------------------------------------------------------------------------
# 알림
# ---------------------------------------------------------------------------
NOTIFICATION_TEMPLATES_USER1 = [
    ("sleep", "어젯밤 수면 리포트가 준비됐어요."),
    ("temperature", "침실 온도가 26도를 넘었어요. 에어컨을 확인해보세요."),
    ("execution", "취침 시간 자동 소등 규칙이 실행되었습니다."),
    ("gesture", "제스처로 침실 조명을 조절했어요."),
    ("timer", "독서 루틴 시간이 되었어요."),
    ("posture", "책상에 오래 앉아 계셨네요. 스트레칭은 어떠세요?"),
]
NOTIFICATION_TEMPLATES_USER2 = [
    ("timer", "헬스장 갈 시간이에요."),
    ("diet", "저녁 식단 기록을 아직 남기지 않으셨어요."),
    ("execution", "인덕션 안전 타이머가 자동으로 꺼졌습니다."),
    ("power", "부엌 인덕션 사용량이 평소보다 많았어요."),
    ("timer", "물 마실 시간이에요."),
]


def gen_notifications(rng: random.Random) -> list[tuple]:
    """user_id, type, message, read, created_at"""
    rows: list[tuple] = []
    for d in timeutil.june_dates():
        date_s = timeutil.fmt_date(d)
        is_recent = (timeutil.MONTH_END - d).days < 2
        for user_id, templates in ((1, NOTIFICATION_TEMPLATES_USER1), (2, NOTIFICATION_TEMPLATES_USER2)):
            n = rng.randint(1, 5)
            for _ in range(n):
                ntype, msg = rng.choice(templates)
                hh, mm = rng.randint(7, 23), rng.randint(0, 59)
                read = 0 if is_recent and rng.random() < 0.6 else 1
                rows.append((user_id, ntype, msg, read, f"{date_s} {hh:02d}:{mm:02d}:00"))
    rows.sort(key=lambda r: r[4])
    return rows


# ---------------------------------------------------------------------------
# 채팅 히스토리
# ---------------------------------------------------------------------------
SYSTEM_MSG = "너는 WaveHome 라이프스타일 에이전트야."

CHAT_TOPICS_USER1 = [
    ("어젯밤 수면 어땠어?", "어젯밤 수면 점수는 {score}점이었어요. 효율은 {eff}%로 양호했고, 새벽에 뒤척임이 조금 있었어요."),
    ("이번 주 전력 사용량 알려줘", "이번 주 전력 사용량은 약 {kwh}kWh였어요. 침실 에어컨 비중이 가장 컸습니다."),
    ("오늘 할 일 뭐 있어?", "오늘은 아침 스트레칭과 취침 전 독서가 예정돼 있어요."),
    ("침실 조명 좀 어둡게 해줘", "침실 조명 밝기를 낮췄어요."),
    ("이번 주 계획 요약해줘", "이번 주는 수면 리듬이 안정적이고, 저녁 독서 루틴을 잘 지키고 계세요."),
]
CHAT_FOLLOWUPS = [
    ("좀 더 자세히 설명해줘", "네, 조금 더 자세히 말씀드릴게요. {detail}"),
    ("그럼 어떻게 하면 좋을까?", "우선 {tip} 을 시도해보시는 걸 추천드려요."),
    ("고마워", "도움이 되었다니 다행이에요. 필요하면 언제든 다시 물어보세요."),
]
CHAT_TOPICS_USER2 = [
    ("오늘 저녁 뭐 먹을지 추천해줘", "최근 식단 기록을 보면 단백질 섭취가 조금 부족했어요. 닭가슴살 샐러드는 어떠세요?"),
    ("이번 달 전력 사용량 어때?", "이번 달 부엌 인덕션 사용량이 평소보다 늘었어요. 저녁 요리 시간이 길어진 것 같아요."),
    ("헬스장 스케줄 확인해줘", "이번 주는 월/수/금 저녁 7시에 헬스장 일정이 있어요."),
    ("거실 선풍기 켜줘", "거실 선풍기를 켰어요."),
]


def _turn_count(rng: random.Random) -> int:
    return rng.randint(1, 10)


def gen_chat_histories(rng: random.Random) -> list[tuple]:
    """id, user_id, title, created_at, updated_at, message(json)"""
    rows: list[tuple] = []
    dates = timeutil.june_dates()
    conv_id = 1

    def build_conv(user_id: int, d, topic_bank, title_prefix) -> tuple:
        nonlocal conv_id
        question, answer_tpl = rng.choice(topic_bank)
        score = rng.randint(65, 95)
        eff = rng.randint(78, 95)
        kwh = round(rng.uniform(2.5, 6.5), 1)
        answer = answer_tpl.format(score=score, eff=eff, kwh=kwh)

        hh, mm = rng.randint(8, 22), rng.randint(0, 59)
        start_ts = f"{timeutil.fmt_date(d)} {hh:02d}:{mm:02d}:00"
        messages = [
            {"role": "system", "content": SYSTEM_MSG},
            {"role": "user", "content": question},
            {"role": "assistant", "content": answer},
        ]
        n_turns = _turn_count(rng)
        cur_minute = mm
        last_ts = start_ts
        for _ in range(n_turns - 1):
            fu_q, fu_a_tpl = rng.choice(CHAT_FOLLOWUPS)
            detail = "센서 데이터 기준으로 판단한 결과예요."
            tip = "취침 30분 전 조명을 낮추는 것"
            messages.append({"role": "user", "content": fu_q})
            messages.append({"role": "assistant", "content": fu_a_tpl.format(detail=detail, tip=tip)})
            cur_minute = (cur_minute + rng.randint(1, 5)) % 60
        updated_ts = start_ts
        title = f"{title_prefix}: {question[:20]}"
        row = (conv_id, user_id, title, start_ts, updated_ts, json.dumps({"messages": messages}, ensure_ascii=False))
        conv_id += 1
        return row

    for _ in range(20):
        d = rng.choice(dates)
        rows.append(build_conv(1, d, CHAT_TOPICS_USER1, "김건강"))
    for _ in range(12):
        d = rng.choice(dates)
        rows.append(build_conv(2, d, CHAT_TOPICS_USER2, "박헬스"))

    rows.sort(key=lambda r: r[3])
    # id 재부여(정렬 후 1..N 유지)
    rows = [(i + 1, r[1], r[2], r[3], r[4], r[5]) for i, r in enumerate(rows)]
    return rows
