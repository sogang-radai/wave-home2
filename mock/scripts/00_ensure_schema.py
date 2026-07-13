#!/usr/bin/env python3
"""기존 mock/data/mock.db 를 데이터 유지한 채 스키마만 보강한다.

- push_subscription, schema_version
- sleep_plan / user_action_log / goal* / power_report_embedding (서버 migration v2~v7)
- 누락 인덱스
- vec_* 가상 테이블 (sqlite-vec 로드 성공 시)

실행:
    uv run --with sqlite-vec mock/scripts/00_ensure_schema.py
    # 또는
    python3 mock/scripts/00_ensure_schema.py
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

from lib import schema

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "mock" / "data" / "mock.db"


def main() -> None:
    if not DB_PATH.exists():
        raise SystemExit(f"mock.db 없음: {DB_PATH}\n먼저 01_gen_raw_data.py 를 실행하세요.")

    conn = sqlite3.connect(DB_PATH)
    vec_ready = schema.ensure_runtime_schema(conn, with_vec=True)

    tables = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
    ).fetchall()
    vec_tables = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'vec_%' ORDER BY name"
    ).fetchall()
    version = conn.execute("SELECT version, description FROM schema_version ORDER BY version DESC LIMIT 1").fetchone()

    conn.close()

    print("=== 00_ensure_schema 완료 ===")
    print(f"db: {DB_PATH}")
    print(f"schema_version: {version}")
    print(f"테이블 수: {len(tables)}")
    print(f"vec_* 테이블: {len(vec_tables)}/{len(schema.VEC_TABLES)} (ready={vec_ready})")
    if vec_tables:
        for (name,) in vec_tables:
            print(f"  - {name}")


if __name__ == "__main__":
    main()
