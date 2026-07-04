#!/usr/bin/env python3
"""bin/data/mock.db 의 power_energy 를 훑어보기 위한 plot 생성기.

docs/images/power/ 에 PNG 몇 장을 저장한다. 한글 폰트 이슈를 피하려고
라벨은 영어로 쓴다.

사용:
    uv run --with matplotlib scripts/plot_power.py
"""

from __future__ import annotations

import sqlite3
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates
import matplotlib.pyplot as plt

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "bin" / "data" / "mock.db"
OUT_DIR = REPO_ROOT / "docs" / "images" / "power"

# 플러그 설명 -> 짧은 영어 라벨.
APPLIANCE_LABEL = {
    "에어컨": "Aircon (living)",
    "거실 스마트 플러그2 - 선풍기": "Fan (living)",
    "부엌 스마트 플러그 - 선풍기": "Fan (kitchen)",
    "컴퓨터": "PC (bedroom)",
}


def label_for(description: str) -> str:
    for key, lbl in APPLIANCE_LABEL.items():
        if key in description:
            return lbl
    return description


def load_plugs(conn: sqlite3.Connection) -> list[tuple[int, str]]:
    rows = conn.execute(
        "SELECT id, description FROM device WHERE class='tuya_ep2h' ORDER BY id"
    ).fetchall()
    return [(r[0], label_for(r[1])) for r in rows]


def fig_daily_by_device(conn: sqlite3.Connection) -> Path:
    """24h 단위: 장치별 일별 kWh 누적 막대 + 합산 라인."""
    plugs = load_plugs(conn)
    days = [
        r[0]
        for r in conn.execute(
            "SELECT DISTINCT time_start FROM power_energy WHERE granularity='24h'"
            " AND device_id IS NULL ORDER BY time_start"
        )
    ]
    x = [datetime.strptime(d, "%Y-%m-%d") for d in days]

    fig, ax = plt.subplots(figsize=(12, 5))
    bottom = [0.0] * len(days)
    for dev_id, lbl in plugs:
        by_day = {
            r[0]: r[1] / 1000.0
            for r in conn.execute(
                "SELECT time_start, energy_wh FROM power_energy"
                " WHERE granularity='24h' AND device_id=?",
                (dev_id,),
            )
        }
        vals = [by_day.get(d, 0.0) for d in days]
        ax.bar(x, vals, bottom=bottom, width=0.8, label=lbl)
        bottom = [b + v for b, v in zip(bottom, vals)]

    ax.plot(x, bottom, color="black", marker="o", ms=3, lw=1, label="Total (plugs)")
    ax.set_title("Daily energy by device (24h rollup)")
    ax.set_ylabel("kWh / day")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d"))
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    fig.autofmt_xdate()
    out = OUT_DIR / "daily_by_device.png"
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    plt.close(fig)
    return out


def fig_day_profile(conn: sqlite3.Connection, day: str, tag: str) -> Path:
    """하루 5m 프로파일: 장치별 평균 전력(W)."""
    plugs = load_plugs(conn)
    fig, ax = plt.subplots(figsize=(12, 5))
    for dev_id, lbl in plugs:
        rows = conn.execute(
            "SELECT time_start, energy_wh FROM power_energy"
            " WHERE granularity='5m' AND device_id=? AND time_start LIKE ?"
            " ORDER BY time_start",
            (dev_id, f"{day}%"),
        ).fetchall()
        if not rows:
            continue
        # off 로 생략된 구간은 선으로 잇지 않도록 5분 넘는 간격에 None 을 끼운다.
        ts: list[datetime | None] = []
        watt: list[float | None] = []
        prev: datetime | None = None
        for r in rows:
            cur = datetime.strptime(r[0], "%Y-%m-%d %H:%M:%S")
            if prev is not None and (cur - prev).total_seconds() > 5 * 60 + 1:
                ts.append(None)
                watt.append(None)
            ts.append(cur)
            watt.append(r[1] / (5 / 60.0))  # energy_wh(5분) -> 평균 W
            prev = cur
        ax.plot(ts, watt, lw=1, label=lbl)

    ax.set_title(f"Power profile {day} ({tag}) - 5m buckets")
    ax.set_ylabel("Average power (W)")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=2))
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(alpha=0.3)
    fig.autofmt_xdate()
    out = OUT_DIR / f"day_profile_{tag}.png"
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    plt.close(fig)
    return out


def fig_hourly_weekday_vs_weekend(conn: sqlite3.Connection) -> Path:
    """평일 vs 주말: 시간대별 평균 합산 전력(W)."""
    rows = conn.execute(
        "SELECT time_start, energy_wh FROM power_energy"
        " WHERE granularity='1h' AND device_id IS NULL"
    ).fetchall()
    wk = [0.0] * 24
    wke = [0.0] * 24
    wk_n = [0] * 24
    wke_n = [0] * 24
    for ts, e in rows:
        dt = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S")
        watt = e  # 1h energy_wh == 평균 W
        if dt.weekday() >= 5:
            wke[dt.hour] += watt
            wke_n[dt.hour] += 1
        else:
            wk[dt.hour] += watt
            wk_n[dt.hour] += 1
    wk_avg = [wk[h] / wk_n[h] if wk_n[h] else 0 for h in range(24)]
    wke_avg = [wke[h] / wke_n[h] if wke_n[h] else 0 for h in range(24)]

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(range(24), wk_avg, marker="o", ms=4, label="Weekday")
    ax.plot(range(24), wke_avg, marker="s", ms=4, label="Weekend")
    ax.set_title("Avg total power by hour: weekday vs weekend (plugs)")
    ax.set_xlabel("Hour of day")
    ax.set_ylabel("Average power (W)")
    ax.set_xticks(range(0, 24, 2))
    ax.axvspan(9, 18, color="gray", alpha=0.08, label="Weekday work hours")
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(alpha=0.3)
    out = OUT_DIR / "hourly_weekday_vs_weekend.png"
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    plt.close(fig)
    return out


def fig_aircon_vs_heat(conn: sqlite3.Connection) -> Path:
    """에어컨 일별 kWh (7월 근접 램프)."""
    aid = conn.execute(
        "SELECT id FROM device WHERE description LIKE '%에어컨%'"
    ).fetchone()[0]
    rows = conn.execute(
        "SELECT time_start, energy_wh FROM power_energy"
        " WHERE granularity='24h' AND device_id=? ORDER BY time_start",
        (aid,),
    ).fetchall()
    x = [datetime.strptime(r[0], "%Y-%m-%d") for r in rows]
    y = [r[1] / 1000.0 for r in rows]

    fig, ax = plt.subplots(figsize=(12, 5))
    ax.bar(x, y, width=0.8, color="tab:red", alpha=0.8)
    ax.set_title("Aircon daily energy (ramps up toward July)")
    ax.set_ylabel("kWh / day")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d"))
    ax.grid(axis="y", alpha=0.3)
    fig.autofmt_xdate()
    out = OUT_DIR / "aircon_daily.png"
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    plt.close(fig)
    return out


def pick_days(conn: sqlite3.Connection) -> tuple[str, str]:
    """사용량이 큰(대표적인) 평일/주말 하루씩 고른다."""
    rows = conn.execute(
        "SELECT time_start, energy_wh FROM power_energy WHERE granularity='24h'"
        " AND device_id IS NULL"
    ).fetchall()
    weekday = max(
        (r for r in rows if datetime.strptime(r[0], "%Y-%m-%d").weekday() < 5),
        key=lambda r: r[1],
    )[0]
    weekend = max(
        (r for r in rows if datetime.strptime(r[0], "%Y-%m-%d").weekday() >= 5),
        key=lambda r: r[1],
    )[0]
    return weekday, weekend


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    try:
        weekday, weekend = pick_days(conn)
        outs = [
            fig_daily_by_device(conn),
            fig_aircon_vs_heat(conn),
            fig_hourly_weekday_vs_weekend(conn),
            fig_day_profile(conn, weekday, "weekday"),
            fig_day_profile(conn, weekend, "weekend"),
        ]
    finally:
        conn.close()
    print("저장 완료:")
    for p in outs:
        print(f"  {p.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
