#!/usr/bin/env python3
"""demo/agent/chat.json → bin/data/demo.db chat_history.

기존 chat_history 행을 모두 지우고, 수집된 S01–S20 대화로 대체한다.
여러 번 실행해도 안전하다(DELETE 후 INSERT).

  python3 demo/scripts/07_load_chat_json_to_db.py
"""

from __future__ import annotations

import json
import sqlite3
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "bin" / "data" / "demo.db"
CHAT_JSON = REPO_ROOT / "demo" / "agent" / "chat.json"


def to_db_time(value: str) -> str:
    """'2026-06-30 21:00:00' 또는 ISO → 'YYYY-MM-DD HH:MM:SS'."""
    s = (value or "").strip()
    if "T" in s:
        s = s.replace("T", " ").split("+", 1)[0].split("Z", 1)[0]
    if len(s) >= 19:
        return s[:19]
    if len(s) == 16:
        return s + ":00"
    return s or "2026-06-30 21:00:00"


def slim_tool_event(te: dict) -> dict:
    """UI/ChatStore가 쓰는 필드만 남긴다."""
    out = {
        "id": te.get("id"),
        "name": te.get("name"),
        "status": te.get("status") or "done",
        "label": te.get("label") or "",
    }
    if te.get("args") is not None:
        out["args"] = te["args"]
    if te.get("result") is not None:
        out["result"] = te["result"]
    if te.get("resultSummary"):
        out["resultSummary"] = te["resultSummary"]
    return out


def conv_to_messages(conv: dict) -> list[dict]:
    messages: list[dict] = []
    for m in conv.get("messages") or []:
        role = m.get("role")
        if role not in ("user", "assistant"):
            continue
        entry: dict = {
            "id": int(m["id"]),
            "role": role,
            "text": m.get("text") or "",
            "createdAt": m.get("createdAt") or "2026-06-30T21:00:00+09:00",
        }
        if role == "assistant":
            entry["status"] = m.get("status") or "done"
            entry["toolEvents"] = [slim_tool_event(t) for t in (m.get("toolEvents") or [])]
            if m.get("model"):
                entry["model"] = m["model"]
        messages.append(entry)
    return messages


def conversations_from_json(path: Path) -> list[tuple]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    convs = doc.get("conversations") or []
    # Stable list order: S01..S20
    convs = sorted(convs, key=lambda c: c.get("id") or "")
    rows: list[tuple] = []
    for i, conv in enumerate(convs, start=1):
        messages = conv_to_messages(conv)
        if not messages:
            continue
        title = conv.get("title") or (messages[0].get("text") or "새 대화")
        if len(title) > 22:
            title = title[:21] + "…"
        created = to_db_time(conv.get("createdAt") or "")
        updated = to_db_time(conv.get("updatedAt") or conv.get("createdAt") or "")
        # Slight stagger so the chat list isn't all the same second.
        if created.endswith("21:00:00"):
            minute = min(59, (i - 1) * 2)
            created = f"2026-06-30 21:{minute:02d}:00"
            updated = created
        rows.append(
            (
                i,
                int(conv.get("userId") or 1),
                title,
                created,
                updated,
                json.dumps(messages, ensure_ascii=False),
            )
        )
    return rows


def main() -> int:
    if not CHAT_JSON.exists():
        print(f"error: missing {CHAT_JSON}", file=sys.stderr)
        return 1
    if not DB_PATH.exists():
        print(f"error: missing {DB_PATH}", file=sys.stderr)
        return 1

    rows = conversations_from_json(CHAT_JSON)
    if not rows:
        print("error: no conversations in chat.json", file=sys.stderr)
        return 1

    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    before = cur.execute("SELECT COUNT(*) FROM chat_history").fetchone()[0]
    cur.execute("DELETE FROM chat_history")
    cur.executemany(
        "INSERT INTO chat_history (id, user_id, title, created_at, updated_at, message) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        rows,
    )
    conn.commit()
    after = cur.execute("SELECT COUNT(*) FROM chat_history").fetchone()[0]
    user_counts = cur.execute(
        "SELECT user_id, COUNT(*) FROM chat_history GROUP BY user_id ORDER BY user_id"
    ).fetchall()
    conn.close()

    print(f"chat_history: deleted {before} → inserted {after}")
    for uid, n in user_counts:
        print(f"  user_id={uid}: {n}")
    print(f"source: {CHAT_JSON}")
    print("=== 07_load_chat_json_to_db 완료 ===")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
