"""수면 목업 데이터 확인용 플롯.

생성물 (docs/images/sleep/):
  - hypnogram.png         : 대표 1박 수면단계(hypnogram) + 뒤척임 + 심박/호흡
  - nightly_stages.png    : 30박 단계 구성(스택) + 수면효율
  - weekly_trend.png      : 주간 평균 점수/수면시간

사용:
    uv run --with matplotlib scripts/plot_sleep.py
"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DB = ROOT / "bin" / "data" / "mock.db"
OUT = ROOT / "docs" / "images" / "sleep"
STAGE_ORDER = ["absent", "awake", "rem", "light", "deep"]  # y축(아래=깊음)
STAGE_Y = {s: i for i, s in enumerate(STAGE_ORDER)}
STAGE_COLOR = {"deep": "#1a3d7c", "light": "#4f8fd0", "rem": "#8e6fd0",
               "awake": "#e0a030", "absent": "#bbbbbb"}


def pick_night(con: sqlite3.Connection) -> int:
    # 코골이/뒤척임이 뚜렷한 후반 밤(더운 날)을 대표로.
    row = con.execute(
        "SELECT id FROM sleep_session ORDER BY snore_ratio DESC LIMIT 1"
    ).fetchone()
    return row[0]


def fig_hypnogram(con: sqlite3.Connection) -> None:
    sid = pick_night(con)
    rows = list(con.execute(
        "SELECT time_start, stage_label, toss_mean, hr_mean, br_mean "
        "FROM sleep_stat WHERE granularity='1m' AND session_id=? ORDER BY time_start", (sid,)
    ))
    night = con.execute("SELECT night_date FROM sleep_session WHERE id=?", (sid,)).fetchone()[0]
    ts = [datetime.strptime(r[0], "%Y-%m-%d %H:%M:%S") for r in rows]
    stage_y = [STAGE_Y[r[1]] for r in rows]
    toss = [r[2] for r in rows]
    hr = [r[3] for r in rows]
    br = [r[4] for r in rows]

    fig, (ax0, ax1, ax2) = plt.subplots(3, 1, figsize=(13, 8), sharex=True,
                                        gridspec_kw={"height_ratios": [2, 1, 1.3]})
    # hypnogram (step + colored fill under each stage)
    ax0.step(ts, stage_y, where="post", color="#333", linewidth=0.9)
    for s in STAGE_ORDER:
        xs = [t for t, r in zip(ts, rows) if r[1] == s]
        ys = [STAGE_Y[s]] * len(xs)
        ax0.scatter(xs, ys, s=6, color=STAGE_COLOR[s], label=s)
    ax0.set_yticks(range(len(STAGE_ORDER)))
    ax0.set_yticklabels(STAGE_ORDER)
    ax0.set_title(f"Hypnogram — {night} (session {sid})")
    ax0.legend(loc="upper right", ncol=5, fontsize=8)
    ax0.grid(True, axis="x", alpha=0.3)

    ax1.plot(ts, toss, color="#cc4444", linewidth=0.7)
    ax1.axhline(0.5, color="gray", ls="--", lw=0.8)
    ax1.set_ylim(0, 1.05)
    ax1.set_ylabel("toss index")
    ax1.grid(True, alpha=0.3)

    ax2.plot(ts, hr, color="#c0392b", linewidth=0.8, label="HR (bpm)")
    ax2.plot(ts, br, color="#2980b9", linewidth=0.8, label="BR (rpm)")
    ax2.set_ylabel("HR / BR")
    ax2.legend(loc="upper right", fontsize=8)
    ax2.grid(True, alpha=0.3)
    ax2.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax2.xaxis.set_major_locator(mdates.AutoDateLocator())

    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(OUT / "hypnogram.png", dpi=120)
    plt.close(fig)


def fig_nightly_stages(con: sqlite3.Connection) -> None:
    rows = list(con.execute(
        "SELECT night_date, stage_totals, efficiency FROM sleep_session ORDER BY night_date"
    ))
    dates = [datetime.strptime(r[0], "%Y-%m-%d") for r in rows]
    totals = [json.loads(r[1]) for r in rows]
    eff = [r[2] * 100 for r in rows]

    fig, ax = plt.subplots(figsize=(14, 5))
    bottom = [0.0] * len(rows)
    for s in ["deep", "light", "rem", "awake"]:
        vals = [t.get(s, 0) / 3600.0 for t in totals]
        ax.bar(dates, vals, bottom=bottom, width=0.8, color=STAGE_COLOR[s], label=s)
        bottom = [b + v for b, v in zip(bottom, vals)]
    ax.set_ylabel("hours in bed by stage")
    ax.set_title("Nightly sleep composition (30 nights)")
    ax.legend(loc="upper left", ncol=4, fontsize=8)

    ax2 = ax.twinx()
    ax2.plot(dates, eff, color="#111", marker="o", ms=3, lw=1.0, label="efficiency %")
    ax2.set_ylim(70, 100)
    ax2.set_ylabel("sleep efficiency (%)")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d"))
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(OUT / "nightly_stages.png", dpi=120)
    plt.close(fig)


def fig_weekly_trend(con: sqlite3.Connection) -> None:
    rows = list(con.execute(
        "SELECT period_start, metrics FROM sleep_report WHERE period='weekly' ORDER BY period_start"
    ))
    weeks = [r[0] for r in rows]
    m = [json.loads(r[1]) for r in rows]
    score = [d["avg_score"] for d in m]
    asleep_h = [d["avg_asleep_min"] / 60.0 for d in m]

    fig, ax = plt.subplots(figsize=(10, 4.5))
    ax.plot(weeks, score, color="#8e44ad", marker="o", label="avg score")
    ax.set_ylabel("avg sleep score")
    ax.set_ylim(60, 100)
    ax2 = ax.twinx()
    ax2.plot(weeks, asleep_h, color="#16a085", marker="s", label="avg asleep (h)")
    ax2.set_ylabel("avg asleep (hours)")
    ax.set_title("Weekly sleep trend")
    ax.grid(True, alpha=0.3)
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(OUT / "weekly_trend.png", dpi=120)
    plt.close(fig)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(DB)
    fig_hypnogram(con)
    fig_nightly_stages(con)
    fig_weekly_trend(con)
    con.close()
    print(f"saved plots to {OUT}")


if __name__ == "__main__":
    main()
