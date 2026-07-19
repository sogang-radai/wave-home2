#!/usr/bin/env python3
"""기존 demo.db 의 power_energy / power_report 만 교체한다(수면·채팅 등 유지).

- DELETE power_report (+ vec_power_report)
- DELETE power_energy
- generate_power_energy 로 5플러그(전자레인지 포함) + NULL 합산 재삽입
- NULL 24h 합 == 플러그 합 검증

실행:
    python3 demo/scripts/01c_regen_power_energy.py
"""

from __future__ import annotations

import sqlite3
import sys
from pathlib import Path

from _lib import devices, power_model, schema

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "bin" / "data" / "demo.db"
PLUG_IDS = (6, 7, 8, 9, 14)


def _plug_pk_by_appliance(conn: sqlite3.Connection) -> dict[str, int]:
    """device 행을 hex(PK=enumerate) 매핑으로 플러그 appliance 키를 찾는다."""
    cur = conn.cursor()
    rows = cur.execute("SELECT id, name, description FROM device").fetchall()
    by_id = {r[0]: r for r in rows}
    out: dict[str, int] = {}
    for hex_id, appliance in devices.PLUG_HEX_TO_APPLIANCE.items():
        # build_device_rows: JSON 순서 enumerate → PK. hex_id(n) 의 n 이 PK.
        pk = int(hex_id, 16)
        if pk not in by_id:
            raise RuntimeError(f"device PK {pk} ({appliance}, {hex_id}) 없음")
        out[appliance] = pk
    missing = set(devices.PLUG_HEX_TO_APPLIANCE.values()) - set(out)
    if missing:
        raise RuntimeError(f"플러그 매핑 누락: {missing}")
    return out


def main() -> None:
    if not DB_PATH.exists():
        print(f"DB 없음: {DB_PATH}", file=sys.stderr)
        sys.exit(1)

    conn = sqlite3.connect(DB_PATH)
    vec_ready = schema.ensure_runtime_schema(conn, with_vec=True)
    cur = conn.cursor()

    if vec_ready:
        try:
            cur.execute("DELETE FROM vec_power_report")
        except sqlite3.OperationalError as exc:
            print(f"vec_power_report 삭제 스킵: {exc}")
    cur.execute("DELETE FROM power_report")
    cur.execute("DELETE FROM power_energy")
    conn.commit()
    print("cleared: power_report, power_energy")

    plug_map = _plug_pk_by_appliance(conn)
    print("plug map:", plug_map)
    counts = power_model.generate_power_energy(conn, plug_map)
    print("inserted power_energy:", counts)

    null_wh = cur.execute(
        "SELECT COALESCE(SUM(energy_wh),0) FROM power_energy "
        "WHERE device_id IS NULL AND granularity='24h'"
    ).fetchone()[0]
    five_wh = cur.execute(
        f"SELECT COALESCE(SUM(energy_wh),0) FROM power_energy "
        f"WHERE device_id IN ({','.join('?' * len(PLUG_IDS))}) AND granularity='24h'",
        PLUG_IDS,
    ).fetchone()[0]
    mw_wh = cur.execute(
        "SELECT COALESCE(SUM(energy_wh),0) FROM power_energy "
        "WHERE device_id=14 AND granularity='24h'"
    ).fetchone()[0]

    null_kwh = round(null_wh / 1000, 2)
    five_kwh = round(five_wh / 1000, 2)
    mw_kwh = round(mw_wh / 1000, 2)
    delta = abs(null_wh - five_wh)
    print(f"verify 24h: null={null_kwh} kWh, five_plugs={five_kwh} kWh, microwave={mw_kwh} kWh, |ΔWh|={delta:.4f}")
    if delta > 1.0:
        print("ERROR: NULL 합산과 5플러그 합이 맞지 않습니다.", file=sys.stderr)
        conn.close()
        sys.exit(2)
    if mw_wh <= 0:
        print("ERROR: 전자레인지(device_id=14) 에너지가 없습니다.", file=sys.stderr)
        conn.close()
        sys.exit(2)

    conn.close()
    print("=== 01c_regen_power_energy 완료 ===")


if __name__ == "__main__":
    main()
