#!/usr/bin/env python3
"""데모 채팅을 턴 단위로 수집해 demo/agent/chat.json 에 저장한다.

워크플로(권장):
  1) 시나리오 시작(첫 대사 + 장치 init)
       python3 demo/scripts/06_collect_chat_turns.py start S01
  2) 출력된 assistant 답을 보고 다음 사용자 대사 결정
       python3 demo/scripts/06_collect_chat_turns.py turn S01 "뒤척임은?"
  3) 더 이상 이어갈 필요 없으면
       python3 demo/scripts/06_collect_chat_turns.py done S01

SSE는 바이트 버퍼에 모은 뒤 이벤트 단위로 UTF-8 디코딩한다(청크 경계 한글 깨짐 방지).

실행 전: demo 백엔드(:8510/:8511) + 에이전트(:8512, CORE mock=false).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_PATH = REPO_ROOT / "demo" / "agent" / "chat.json"

AGENT_URL = "http://127.0.0.1:8512"
INTERNAL_URL = "http://127.0.0.1:8511"
ANCHOR_NOW = "2026-06-30 21:00:00"
KST = timezone(timedelta(hours=9))

TOOL_LABELS = {
    "query_db": "DB 조회",
    "rag_search": "메모리 검색",
    "list_devices": "기기 목록 조회",
    "get_device_classes": "기기 종류 조회",
    "get_device_capabilities": "기기 기능 조회",
    "query_device": "기기 조회",
    "control_device": "기기 제어",
    "get_device_state": "기기 상태 조회",
    "get_schedule_tasks": "일정 조회",
    "create_schedule_task": "일정 생성",
    "update_schedule_task": "일정 수정",
    "delete_schedule_task": "일정 삭제",
    "get_alarms": "알람 조회",
    "create_alarm": "알람 생성",
    "update_alarm": "알람 수정",
    "delete_alarm": "알람 삭제",
    "schedule_device_action": "기기 예약",
    "automate_device_action": "자동화 생성",
    "list_schedules": "예약 목록 조회",
    "cancel_schedule": "예약 취소",
    "execute_rule": "자동화 실행",
    "set_rule_enabled": "자동화 on/off",
    "list_events": "이벤트 조회",
    "list_ir_commands": "IR 목록 조회",
    "get_ir_command": "IR 조회",
}


def hex_id(n: int) -> str:
    return f"{n:016x}"


DEV = {
    "fan": hex_id(6),
    "pc": hex_id(7),
    "aircon": hex_id(8),
    "induction": hex_id(9),
    "tv": hex_id(11),
    "bed_light": hex_id(12),
    "living_light": hex_id(13),
    "kitchen_light": hex_id(14),
}

# 첫 대사만 고정. 후속 턴은 답변을 보고 수동으로 넣는다.
SCENARIOS: dict[str, dict[str, Any]] = {
    "S01": {
        "title": "어젯밤 수면",
        "userId": 1,
        "domain": "sleep",
        "first": "어젯밤 잠 어땠어? 점수랑 효율만 짧게.",
    },
    "S02": {
        "title": "18일 폭염 밤",
        "userId": 1,
        "domain": "sleep",
        "first": "18일 밤에 잠이 진짜 별로였던 것 같은데, 그날 데이터 좀 봐줘.",
    },
    "S03": {
        "title": "25일 짧은 수면",
        "userId": 1,
        "domain": "sleep",
        "first": "25일은 모임 때문에 늦게 잤는데, 그날 얼마나 짧게 잤어?",
    },
    "S04": {
        "title": "최근 일주일 수면",
        "userId": 1,
        "domain": "sleep",
        "first": "최근 일주일 수면 어때? 괜찮았던 점이랑 아쉬운 점만.",
    },
    "S05": {
        "title": "10일 재택",
        "userId": 1,
        "domain": "sleep",
        "first": "10일엔 재택근무였는데, 그날 늦게 잤던 흔적이 있어?",
    },
    "S06": {
        "title": "기상 맞춤 알람",
        "userId": 1,
        "domain": "sleep_iot",
        "first": "평일 아침 알람 어떻게 돼 있어? 기상 맞춤이랑 조명 연동도.",
    },
    "S07": {
        "title": "취침 전 조명",
        "userId": 1,
        "domain": "iot",
        "init": [("bed_light", "on", {}), ("bed_light", "brightness", {"brightness": 90})],
        "first": "자려고 하는데 침실 불 지금 어때? 너무 밝으면 좀 낮춰줘.",
    },
    "S08": {
        "title": "외출 전 정리",
        "userId": 1,
        "domain": "iot",
        "init": [("aircon", "on", {}), ("tv", "on", {})],
        "first": "나 나간다. 침실 에어컨이랑 TV 켜져 있으면 꺼줘.",
    },
    "S09": {
        "title": "거실 선풍기·조명",
        "userId": 1,
        "domain": "iot",
        "init": [
            ("fan", "off", {}),
            ("living_light", "on", {}),
            ("living_light", "brightness", {"brightness": 55}),
        ],
        "first": "거실 좀 더운데 선풍기 켜주고, 불도 저녁 느낌으로 해줘.",
    },
    "S10": {
        "title": "제스처 자동화",
        "userId": 1,
        "domain": "iot",
        "first": "침실에서 손동작으로 TV랑 불 다루는 자동화, 지금 뭐가 켜져 있어?",
    },
    "S11": {
        "title": "인덕션 안전 자동화",
        "userId": 1,
        "domain": "iot",
        "init": [("induction", "off", {})],
        "first": "인덕션 안전 타이머 자동화 켜져 있어? 플러그 상태도.",
    },
    "S12": {
        "title": "침실 TV 끄기",
        "userId": 1,
        "domain": "iot",
        "init": [("tv", "on", {})],
        "first": "침실 TV 켜져 있으면 좀 있다 자려고 하니까, 지금 상태 보고 꺼줄 수 있어?",
    },
    "S13": {
        "title": "이번 주 전력",
        "userId": 1,
        "domain": "power",
        "first": "요즘 일주일 전기 얼마나 썼어? 총량이랑 뭐가 많이 먹었는지만.",
    },
    "S14": {
        "title": "18일 에어컨 전력",
        "userId": 1,
        "domain": "power",
        "first": "18일 엄청 더웠는데, 그날 침실 에어컨 전기 많이 나갔지?",
    },
    "S15": {
        "title": "25일 저녁 전력",
        "userId": 1,
        "domain": "power",
        "first": "25일 저녁에 손님 있었거든. 그날 전기 평소랑 달랐어?",
    },
    "S16": {
        "title": "이번 달·대기전력",
        "userId": 1,
        "domain": "power",
        "init": [
            ("fan", "off", {}),
            ("pc", "off", {}),
            ("aircon", "off", {}),
            ("induction", "off", {}),
        ],
        "first": "이번 달 전기 총량이랑, 대기전력 큰 플러그 있으면 알려줘.",
    },
    "S17": {
        "title": "PC 플러그 지금",
        "userId": 1,
        "domain": "power_iot",
        "init": [("pc", "on", {})],
        "first": "침실 컴 플러그 지금 얼마나 먹어? 쓸데없으면 꺼줘.",
    },
    "S18": {
        "title": "수면 일정",
        "userId": 1,
        "domain": "sleep_schedule",
        "first": "내 일정에 수면 관련으로 잡아둔 거 뭐 있어?",
    },
    "S19": {
        "title": "Wave Station 음성 알람",
        "userId": 1,
        "domain": "iot",
        "first": "웨이브 스테이션으로 깨우는 알람 어떻게 돼 있어? 멘트도.",
    },
    "S20": {
        "title": "더운 밤 짧게",
        "userId": 1,
        "domain": "mixed",
        "init": [
            ("aircon", "off", {}),
            ("bed_light", "on", {}),
            ("bed_light", "brightness", {"brightness": 70}),
        ],
        "first": "오늘 밤 더울 것 같은데 침실 에어컨이랑 불 좀 맞춰줘.",
    },
}


def _http_json(
    method: str,
    url: str,
    body: dict | None = None,
    headers: dict | None = None,
    timeout: float = 30.0,
) -> tuple[int, Any]:
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8")
            return resp.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8") if exc.fp else ""
        try:
            return exc.code, json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            return exc.code, {"raw": raw}


def invoke_device(runtime_id: str, device_key: str, action: str, params: dict) -> None:
    device_id = DEV[device_key]
    status, payload = _http_json(
        "POST",
        f"{INTERNAL_URL}/internal/v1/devices/{device_id}/actions/{action}",
        {"userId": 1, "demoRuntimeId": runtime_id, "params": params or {}},
        headers={"X-Wave-Demo-Runtime-Id": runtime_id},
        timeout=10.0,
    )
    if status >= 400:
        print(f"  [init warn] {device_key}.{action} -> HTTP {status} {payload}", file=sys.stderr)


def tool_label(name: str, running: bool, failed: bool) -> str:
    base = TOOL_LABELS.get(name, name)
    if running:
        return f"{base} 중"
    if failed:
        return f"{base} 실패"
    return f"{base} 완료"


def summarize_result(result: Any) -> str:
    if result is None:
        return ""
    if isinstance(result, str):
        return result
    if isinstance(result, bool):
        return "성공" if result else "실패"
    if isinstance(result, (int, float)):
        return str(result)
    if isinstance(result, dict):
        if "count" in result and isinstance(result["count"], (int, float)):
            return f"{int(result['count'])}건"
        if isinstance(result.get("raw"), str):
            return result["raw"]
        if isinstance(result.get("deviceName"), str) and isinstance(result.get("action"), str):
            return f"{result['deviceName']} · {result['action']}"
        if isinstance(result.get("ok"), bool):
            return "성공" if result["ok"] else "실패"
    return ""


def run_chat_turn(
    *,
    chat_history_id: int,
    user_id: int,
    messages: list[dict[str, str]],
    runtime_id: str,
    now: str,
) -> tuple[str, list[dict[str, Any]], str]:
    body = {
        "chatHistoryId": chat_history_id,
        "userId": user_id,
        "messages": messages,
        "context": {"now": now, "demoRuntimeId": runtime_id},
        "stream": True,
    }
    req = urllib.request.Request(
        f"{AGENT_URL}/chat/v1/turns",
        data=json.dumps(body).encode("utf-8"),
        method="POST",
        headers={"Content-Type": "application/json", "Accept": "text/event-stream"},
    )

    content = ""
    model = ""
    tools: dict[str, dict[str, Any]] = {}
    order: list[str] = []

    with urllib.request.urlopen(req, timeout=180.0) as resp:
        # Decode only at SSE event boundaries so UTF-8 multibyte chars are never split.
        byte_buf = b""
        while True:
            chunk = resp.read(4096)
            if not chunk:
                break
            byte_buf += chunk
            while b"\n\n" in byte_buf:
                raw_block, byte_buf = byte_buf.split(b"\n\n", 1)
                try:
                    block = raw_block.decode("utf-8")
                except UnicodeDecodeError:
                    # Incomplete trailing sequence — wait for more bytes.
                    byte_buf = raw_block + b"\n\n" + byte_buf
                    break
                for line in block.splitlines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        continue
                    try:
                        event = json.loads(data)
                    except json.JSONDecodeError:
                        continue
                    et = event.get("type")
                    if et == "tool.start":
                        tid = event.get("id") or f"anon-{len(order)}"
                        tools[tid] = {
                            "id": tid,
                            "name": event.get("name", ""),
                            "status": "running",
                            "label": tool_label(event.get("name", ""), True, False),
                            "args": event.get("args"),
                        }
                        if tid not in order:
                            order.append(tid)
                    elif et == "tool.end":
                        tid = event.get("id") or (order[-1] if order else f"anon-{len(order)}")
                        failed = event.get("ok") is False
                        name = event.get("name", tools.get(tid, {}).get("name", ""))
                        prior = tools.get(tid, {})
                        tools[tid] = {
                            "id": tid,
                            "name": name,
                            "status": "failed" if failed else "done",
                            "label": tool_label(name, False, failed),
                            "args": prior.get("args", event.get("args")),
                            "result": event.get("result"),
                            "resultSummary": summarize_result(event.get("result")),
                        }
                        if tid not in order:
                            order.append(tid)
                    elif et == "message.delta" and event.get("content"):
                        content += event["content"]
                    elif et == "message.completed":
                        content = event.get("content") or content
                        model = event.get("model") or model
                    elif et == "error":
                        raise RuntimeError(f"agent error: {event.get('error')}")

    tool_events = []
    for tid in order:
        te = tools[tid]
        if te.get("status") == "running":
            te["status"] = "done"
            te["label"] = tool_label(te.get("name", ""), False, False)
        cleaned = {k: v for k, v in te.items() if v is not None and v != ""}
        tool_events.append(cleaned)

    if "\ufffd" in content:
        raise RuntimeError("assistant text contains U+FFFD — UTF-8 decode still broken")

    return content, tool_events, model


def iso_from_now(now: str, offset_min: int = 0) -> str:
    dt = datetime.strptime(now, "%Y-%m-%d %H:%M:%S").replace(tzinfo=KST) + timedelta(minutes=offset_min)
    return dt.isoformat(timespec="seconds")


def load_doc() -> dict[str, Any]:
    if OUT_PATH.exists():
        return json.loads(OUT_PATH.read_text(encoding="utf-8"))
    return {
        "anchorNow": ANCHOR_NOW,
        "agentUrl": AGENT_URL,
        "generatedAt": datetime.now(KST).isoformat(timespec="seconds"),
        "conversations": [],
    }


def save_doc(doc: dict[str, Any]) -> None:
    order = list(SCENARIOS.keys())
    by_id = {c["id"]: c for c in doc.get("conversations", [])}
    conversations = [by_id[sid] for sid in order if sid in by_id]
    for sid, conv in by_id.items():
        if sid not in order:
            conversations.append(conv)
    doc["conversations"] = conversations
    doc["generatedAt"] = datetime.now(KST).isoformat(timespec="seconds")
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def get_conv(doc: dict[str, Any], sid: str) -> dict[str, Any] | None:
    for c in doc.get("conversations", []):
        if c["id"] == sid:
            return c
    return None


def agent_messages_from_conv(conv: dict[str, Any]) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    for m in conv.get("messages", []):
        role = m["role"]
        if role not in ("user", "assistant", "system"):
            continue
        text = m.get("text") or ""
        out.append({"role": role, "content": text})
    return out


def next_msg_id(conv: dict[str, Any]) -> int:
    ids = [m.get("id", 0) for m in conv.get("messages", [])]
    return (max(ids) if ids else 0) + 1


def user_turn_count(conv: dict[str, Any]) -> int:
    return sum(1 for m in conv.get("messages", []) if m.get("role") == "user")


def append_turn(conv: dict[str, Any], user_text: str) -> dict[str, Any]:
    sid = conv["id"]
    runtime_id = conv["demoRuntimeId"]
    user_id = conv["userId"]
    chat_history_id = 9000 + int(sid[1:])
    minute = max(0, (len(conv["messages"]) // 2) * 2)

    msg_id = next_msg_id(conv)
    conv["messages"].append({
        "id": msg_id,
        "role": "user",
        "text": user_text,
        "createdAt": iso_from_now(ANCHOR_NOW, minute),
    })
    agent_messages = agent_messages_from_conv(conv)

    print(f"\n=== {sid} turn {user_turn_count(conv)} ===")
    print(f"USER: {user_text}")
    t0 = time.monotonic()
    content, tool_events, model = run_chat_turn(
        chat_history_id=chat_history_id,
        user_id=user_id,
        messages=agent_messages,
        runtime_id=runtime_id,
        now=ANCHOR_NOW,
    )
    elapsed = time.monotonic() - t0
    tool_names = [t.get("name") for t in tool_events]
    print(f"({elapsed:.1f}s) tools={tool_names} model={model}")
    print("--- ASSISTANT ---")
    print(content)
    print("--- END ---")

    conv["messages"].append({
        "id": msg_id + 1,
        "role": "assistant",
        "text": content,
        "status": "done",
        "toolEvents": tool_events,
        "model": model,
        "createdAt": iso_from_now(ANCHOR_NOW, minute + 1),
    })
    conv["updatedAt"] = f"{ANCHOR_NOW[:10]} {ANCHOR_NOW[11:]}"
    conv.pop("done", None)
    return conv


def cmd_start(sid: str) -> int:
    sid = sid.upper()
    if sid not in SCENARIOS:
        print(f"unknown scenario {sid}", file=sys.stderr)
        return 1
    sc = SCENARIOS[sid]
    doc = load_doc()
    if get_conv(doc, sid) is not None:
        print(f"{sid} already exists. Use 'turn' or delete it first.", file=sys.stderr)
        return 1

    # Fresh twin per start so leftover plug/alarm state from prior runs cannot leak in.
    runtime_id = f"chat-seed-{sid.lower()}-{int(time.time())}"
    for device_key, action, params in sc.get("init") or []:
        invoke_device(runtime_id, device_key, action, params)
        time.sleep(0.05)

    title = sc["first"]
    if len(title) > 22:
        title = title[:21] + "…"
    conv = {
        "id": sid,
        "userId": sc["userId"],
        "title": title,
        "domain": sc.get("domain"),
        "scenarioTitle": sc["title"],
        "demoRuntimeId": runtime_id,
        "contextNow": ANCHOR_NOW,
        "createdAt": f"{ANCHOR_NOW[:10]} {ANCHOR_NOW[11:]}",
        "updatedAt": f"{ANCHOR_NOW[:10]} {ANCHOR_NOW[11:]}",
        "messages": [],
    }
    append_turn(conv, sc["first"])
    doc.setdefault("conversations", []).append(conv)
    save_doc(doc)
    print(f"\nsaved {OUT_PATH}  ({sid} open, turns={user_turn_count(conv)})")
    return 0


def cmd_turn(sid: str, text: str) -> int:
    sid = sid.upper()
    doc = load_doc()
    conv = get_conv(doc, sid)
    if conv is None:
        print(f"{sid} not started. Run: start {sid}", file=sys.stderr)
        return 1
    if conv.get("done"):
        print(f"{sid} already marked done.", file=sys.stderr)
        return 1
    append_turn(conv, text)
    save_doc(doc)
    print(f"\nsaved {OUT_PATH}  ({sid} open, turns={user_turn_count(conv)})")
    return 0


def cmd_done(sid: str) -> int:
    sid = sid.upper()
    doc = load_doc()
    conv = get_conv(doc, sid)
    if conv is None:
        print(f"{sid} not found", file=sys.stderr)
        return 1
    conv["done"] = True
    save_doc(doc)
    print(f"{sid} done. turns={user_turn_count(conv)} messages={len(conv['messages'])}")
    return 0


def cmd_status() -> int:
    doc = load_doc()
    print(f"file: {OUT_PATH} exists={OUT_PATH.exists()}")
    for sid in SCENARIOS:
        conv = get_conv(doc, sid)
        if not conv:
            print(f"  {sid}: (missing)")
            continue
        flag = "done" if conv.get("done") else "open"
        n_fffd = sum((m.get("text") or "").count("\ufffd") for m in conv["messages"])
        print(f"  {sid}: {flag} turns={user_turn_count(conv)} fffd={n_fffd}")
    return 0


def cmd_reset() -> int:
    if OUT_PATH.exists():
        OUT_PATH.unlink()
        print(f"deleted {OUT_PATH}")
    else:
        print("no file")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_start = sub.add_parser("start", help="Start scenario with first user message")
    p_start.add_argument("scenario")

    p_turn = sub.add_parser("turn", help="Append a user turn after reviewing the last answer")
    p_turn.add_argument("scenario")
    p_turn.add_argument("text")

    p_done = sub.add_parser("done", help="Mark scenario complete")
    p_done.add_argument("scenario")

    sub.add_parser("status", help="Show collection status")
    sub.add_parser("reset", help="Delete chat.json")

    args = parser.parse_args()
    if args.cmd == "start":
        return cmd_start(args.scenario)
    if args.cmd == "turn":
        return cmd_turn(args.scenario, args.text)
    if args.cmd == "done":
        return cmd_done(args.scenario)
    if args.cmd == "status":
        return cmd_status()
    if args.cmd == "reset":
        return cmd_reset()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
