#!/usr/bin/env python3
"""데모 영상 촬영용 핫키 타이핑 툴 (Windows).

F1~F10 을 누르면 PHRASES 배열의 문구가 현재 포커스된 창에
실제 타이핑하듯 한 글자씩 입력됩니다. Esc 로 종료합니다.

설치 (Windows PowerShell / CMD):
  pip install keyboard

실행 (전역 핫키가 안 잡히면 관리자 권한으로):
  python scripts/demo_hotkey_type.py

주의:
  - 입력될 텍스트 칸(채팅 입력창 등)을 먼저 클릭해 포커스를 두세요.
  - 타이핑 중에는 다른 F키 입력이 무시됩니다.
  - Ctrl+Shift+C 로 진행 중인 타이핑을 취소할 수 있습니다.
"""

from __future__ import annotations

import random
import sys
import threading
import time

try:
    import keyboard
except ImportError:
    print("keyboard 패키지가 필요합니다:  pip install keyboard", file=sys.stderr)
    sys.exit(1)


# ── 여기 문구만 원하는 데모 대본으로 바꾸면 됩니다 (F1=0 … F10=9) ─────────────
PHRASES: list[str] = [
    "지금 집 전체 전력 사용량이랑 시간당 요금 알려줘.",
    "거실 선풍기도 꺼줘. 끈 다음에 집 전체 시간당 요금 다시 알려줘.",
    "현재 연결된 모든 기기를 방별로 정리해서 알려줘.",
    "침실 조명 밝기 40%로 낮춰줘.",
    "오늘 밤 수면 리포트 요약해줘.",
    "주간 계획에 취침 11시 전으로 목표 코칭 해줘.",
    "에어컨 끄고 선풍기만 켜둔 상태로 요금 차이 대략 알려줘.",
    "Wave Station으로 학습한 적외선 명령 목록 보여줘.",
    "내일 아침 7시에 침실 조명 켜지도록 예약해줘.",
    "오늘 할 일 중에서 수면이랑 관련된 것만 알려줘.",
]

# 글자당 기본 지연(초). 너무 빠르면 영상에서 붙여넣기처럼 보입니다.
BASE_DELAY_SEC = 0.035
# 글자마다 랜덤으로 더하는 지연 범위(초) — 사람처럼 불규칙하게.
JITTER_SEC = (0.01, 0.045)
# 문장부호·줄바꿈 뒤 살짝 더 멈춤.
PAUSE_AFTER = {
    ".": 0.18,
    "!": 0.18,
    "?": 0.2,
    "。": 0.18,
    "…": 0.22,
    ",": 0.08,
    "，": 0.08,
    "\n": 0.25,
}
# 핫키 직후 포커스 안정화 대기
FOCUS_SETTLE_SEC = 0.25


_typing_lock = threading.Lock()
_cancel = threading.Event()


def _type_like_human(text: str) -> None:
    _cancel.clear()
    time.sleep(FOCUS_SETTLE_SEC)
    for ch in text:
        if _cancel.is_set():
            print("\n[취소됨]")
            return
        if ch == "\n":
            keyboard.press_and_release("enter")
        elif ch == "\t":
            keyboard.press_and_release("tab")
        else:
            keyboard.write(ch, delay=0)
        time.sleep(BASE_DELAY_SEC + random.uniform(*JITTER_SEC))
        extra = PAUSE_AFTER.get(ch)
        if extra:
            time.sleep(extra)
    print("  → 입력 완료")


def _on_hotkey(index: int) -> None:
    if index < 0 or index >= len(PHRASES):
        return
    phrase = PHRASES[index]
    if not phrase:
        print(f"F{index + 1}: (빈 문구 — 건너뜀)")
        return
    if not _typing_lock.acquire(blocking=False):
        print(f"F{index + 1}: 이미 타이핑 중 — 무시 (취소: Ctrl+Shift+C)")
        return

    def worker() -> None:
        try:
            preview = phrase.replace("\n", "⏎")
            if len(preview) > 60:
                preview = preview[:57] + "..."
            print(f"F{index + 1}: {preview}")
            _type_like_human(phrase)
        finally:
            _typing_lock.release()

    threading.Thread(target=worker, daemon=True).start()


def _request_cancel() -> None:
    if _typing_lock.locked():
        _cancel.set()
        print("취소 요청…")


def main() -> None:
    if sys.platform != "win32":
        print("경고: Windows 기준으로 작성됐습니다. 현재 OS에서는 핫키/입력이 다를 수 있습니다.")

    n = min(10, len(PHRASES))
    print("데모 핫키 타이핑 준비됨")
    print("  F1~F{}  → 저장된 문구 타이핑".format(n))
    print("  Ctrl+Shift+C → 타이핑 취소")
    print("  Esc → 종료")
    print("문구:")
    for i in range(n):
        label = PHRASES[i].replace("\n", "⏎") or "(비어 있음)"
        if len(label) > 72:
            label = label[:69] + "..."
        print(f"  F{i + 1}: {label}")
    print()

    for i in range(n):
        keyboard.add_hotkey(f"f{i + 1}", lambda idx=i: _on_hotkey(idx), suppress=True)

    keyboard.add_hotkey("ctrl+shift+c", _request_cancel, suppress=True)
    keyboard.wait("esc")
    print("종료합니다.")


if __name__ == "__main__":
    main()
