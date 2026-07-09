#!/usr/bin/env python3
"""Analyze Samsung Health sleep export and compare with sleep-net radar CSVs."""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch

STAGE_CODE = {
    40001: "awake",
    40002: "light",
    40003: "deep",
    40004: "rem",
}
STAGE_COLORS = {
    "awake": "#f28e2b",
    "light": "#76b7b2",
    "deep": "#4e79a7",
    "rem": "#b07aa1",
}
RADAR_STATUS_COLORS = {
    "absent": "#e15759",
    "awake": "#f28e2b",
    "asleep": "#59a14f",
}


def parse_samsung_csv(path: Path) -> Tuple[List[str], List[Dict[str, str]]]:
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    if len(lines) < 2:
        raise ValueError(f"unexpected samsung csv: {path}")
    header = lines[1].split(",")
    rows: List[Dict[str, str]] = []
    for line in lines[2:]:
        if not line.strip():
            continue
        cols = line.split(",")
        rows.append(dict(zip(header, cols + [""] * max(0, len(header) - len(cols)))))
    return header, rows


def parse_ts(value: str) -> datetime:
    value = value.strip()
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(value, fmt)
        except ValueError:
            continue
    raise ValueError(f"bad timestamp: {value!r}")


@dataclass
class StageSegment:
    start: datetime
    end: datetime
    stage: str
    sleep_id: str


@dataclass
class SleepSession:
    sleep_id: str
    bed_time: datetime
    wake_time: datetime
    duration_min: int
    sleep_score: Optional[int]
    efficiency: Optional[float]
    sleep_latency_min: Optional[float]
    rem_min: Optional[int]
    light_min: Optional[int]
    movement_awakening: Optional[float]
    mental_recovery: Optional[float]
    physical_recovery: Optional[float]


def load_sleep_sessions(export_dir: Path) -> List[SleepSession]:
    _, rows = parse_samsung_csv(export_dir / "com.samsung.shealth.sleep.20260707194622.csv")
    sessions: List[SleepSession] = []
    for row in rows:
        sid = row["com.samsung.health.sleep.datauuid"]
        latency_ms = row.get("sleep_latency") or ""
        sessions.append(
            SleepSession(
                sleep_id=sid,
                bed_time=parse_ts(row["com.samsung.health.sleep.start_time"]),
                wake_time=parse_ts(row["com.samsung.health.sleep.end_time"]),
                duration_min=int(row["sleep_duration"] or 0),
                sleep_score=int(row["sleep_score"]) if row.get("sleep_score") else None,
                efficiency=float(row["efficiency"]) if row.get("efficiency") else None,
                sleep_latency_min=int(latency_ms) / 60000 if latency_ms else None,
                rem_min=int(row["total_rem_duration"]) if row.get("total_rem_duration") else None,
                light_min=int(row["total_light_duration"]) if row.get("total_light_duration") else None,
                movement_awakening=float(row["movement_awakening"])
                if row.get("movement_awakening")
                else None,
                mental_recovery=float(row["mental_recovery"]) if row.get("mental_recovery") else None,
                physical_recovery=float(row["physical_recovery"])
                if row.get("physical_recovery")
                else None,
            )
        )
    return sessions


def load_stage_segments(export_dir: Path, sleep_id: str) -> List[StageSegment]:
    _, rows = parse_samsung_csv(export_dir / "com.samsung.health.sleep_stage.20260707194622.csv")
    segments: List[StageSegment] = []
    for row in rows:
        if row.get("sleep_id") != sleep_id:
            continue
        code = int(row["stage"])
        segments.append(
            StageSegment(
                start=parse_ts(row["start_time"]),
                end=parse_ts(row["end_time"]),
                stage=STAGE_CODE.get(code, f"unknown_{code}"),
                sleep_id=sleep_id,
            )
        )
    segments.sort(key=lambda s: s.start)
    return segments


def stage_durations(segments: Sequence[StageSegment]) -> Dict[str, float]:
    totals: Dict[str, float] = Counter()
    for seg in segments:
        totals[seg.stage] += (seg.end - seg.start).total_seconds() / 60.0
    return dict(totals)


def asleep_ratio_from_stages(segments: Sequence[StageSegment], t0: datetime, t1: datetime) -> float:
    asleep_sec = 0.0
    total_sec = (t1 - t0).total_seconds()
    if total_sec <= 0:
        return float("nan")
    for seg in segments:
        if seg.stage == "awake":
            continue
        overlap_start = max(seg.start, t0)
        overlap_end = min(seg.end, t1)
        if overlap_end > overlap_start:
            asleep_sec += (overlap_end - overlap_start).total_seconds()
    return asleep_sec / total_sec


@dataclass
class RadarRow:
    ts: datetime
    status: str
    toss_index: float
    toss_valid: bool


def load_radar_csv(path: Path) -> List[RadarRow]:
    rows: List[RadarRow] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for raw in csv.DictReader(handle):
            if raw.get("warmup") == "1" or raw.get("connected") != "1":
                continue
            if raw.get("status_label") in (None, "", "?"):
                continue
            toss = float(raw["toss_index"]) if raw.get("toss_index") else np.nan
            rows.append(
                RadarRow(
                    ts=datetime.strptime(f"{raw['date']} {raw['time']}", "%Y-%m-%d %H:%M:%S"),
                    status=raw["status_label"],
                    toss_index=toss,
                    toss_valid=raw.get("toss_valid") == "1",
                )
            )
    return rows


def radar_status_ratio(rows: Sequence[RadarRow]) -> Dict[str, float]:
    counts = Counter(r.status for r in rows)
    total = sum(counts.values()) or 1
    return {k: counts[k] / total for k in ("absent", "awake", "asleep")}


def plot_stage_timeline(segments: Sequence[StageSegment], session: SleepSession, out_path: Path) -> None:
    stage_to_y = {"awake": 0, "light": 1, "deep": 2, "rem": 3}
    fig, ax = plt.subplots(figsize=(16, 4), constrained_layout=True)
    fig.suptitle(
        f"Samsung Health sleep stages — {session.bed_time:%m-%d %H:%M} → {session.wake_time:%m-%d %H:%M}",
        fontsize=13,
        fontweight="bold",
    )
    for seg in segments:
        y = stage_to_y.get(seg.stage, -1)
        if y < 0:
            continue
        ax.barh(
            y,
            (seg.end - seg.start).total_seconds() / 3600,
            left=mdates.date2num(seg.start),
            height=0.7,
            color=STAGE_COLORS.get(seg.stage, "#999"),
            align="center",
        )
    ax.xaxis_date()
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax.set_yticks([0, 1, 2, 3])
    ax.set_yticklabels(["awake", "light", "deep", "rem"])
    ax.set_xlabel("time (KST)")
    ax.grid(True, axis="x", alpha=0.25)
    handles = [Patch(facecolor=STAGE_COLORS[s], label=s) for s in ("awake", "light", "deep", "rem")]
    ax.legend(handles=handles, loc="upper right", ncol=4, fontsize=9)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_stage_pie(durations: Dict[str, float], out_path: Path) -> None:
    labels = [s for s in ("awake", "light", "deep", "rem") if durations.get(s, 0) > 0]
    sizes = [durations[s] for s in labels]
    fig, ax = plt.subplots(figsize=(6, 6), constrained_layout=True)
    ax.pie(
        sizes,
        labels=[f"{s}\n{durations[s]:.0f}m" for s in labels],
        colors=[STAGE_COLORS[s] for s in labels],
        autopct="%1.1f%%",
        startangle=90,
    )
    ax.set_title("Stage duration (Samsung)", fontsize=12, fontweight="bold")
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_comparison(
    segments: Sequence[StageSegment],
    radar_rows: Dict[str, List[RadarRow]],
    radar_window: Tuple[datetime, datetime],
    out_path: Path,
) -> None:
    t0, t1 = radar_window
    fig, axes = plt.subplots(len(radar_rows) + 1, 1, figsize=(16, 2.2 * (len(radar_rows) + 1)), sharex=True)
    if len(radar_rows) == 0:
        return
    if len(radar_rows) == 0:
        axes = [axes]

    ax0 = axes[0]
    stage_to_y = {"awake": 0, "light": 1, "deep": 2, "rem": 3}
    for seg in segments:
        if seg.end <= t0 or seg.start >= t1:
            continue
        y = stage_to_y.get(seg.stage, -1)
        if y < 0:
            continue
        clip_start = max(seg.start, t0)
        clip_end = min(seg.end, t1)
        ax0.barh(
            y,
            (clip_end - clip_start).total_seconds() / 3600,
            left=mdates.date2num(clip_start),
            height=0.6,
            color=STAGE_COLORS.get(seg.stage, "#999"),
        )
    ax0.set_yticks([0, 1, 2, 3])
    ax0.set_yticklabels(["awake", "light", "deep", "rem"])
    ax0.set_ylabel("Samsung")
    ax0.set_title("Samsung stages vs radar status (radar window only)", fontweight="bold")
    ax0.grid(True, axis="x", alpha=0.2)

    status_to_y = {"absent": 0, "awake": 1, "asleep": 2}
    for ax, (key, rows) in zip(axes[1:], radar_rows.items()):
        pts = [(r.ts, status_to_y[r.status]) for r in rows if t0 <= r.ts < t1]
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.scatter(xs, ys, s=4, c=[RADAR_STATUS_COLORS.get(r.status, "#999") for r in rows if t0 <= r.ts < t1], alpha=0.5)
        ax.set_yticks([0, 1, 2])
        ax.set_yticklabels(["absent", "awake", "asleep"])
        ax.set_ylabel(f"radar {key}")
        ax.grid(True, axis="x", alpha=0.2)

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    axes[-1].set_xlabel("time (KST)")
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def fmt_minutes(m: float) -> str:
    h = int(m // 60)
    r = int(round(m % 60))
    if h:
        return f"{h}h {r}m"
    return f"{r}m"


def write_summary(
    session: SleepSession,
    segments: Sequence[StageSegment],
    durations: Dict[str, float],
    radar_rows: Dict[str, List[RadarRow]],
    out_path: Path,
) -> None:
    in_bed_min = (session.wake_time - session.bed_time).total_seconds() / 60
    asleep_min = sum(durations.get(s, 0) for s in ("light", "deep", "rem"))
    radar_t0 = min(r.ts for rows in radar_rows.values() for r in rows)
    radar_t1 = max(r.ts for rows in radar_rows.values() for r in rows)

    lines = [
        "# Samsung Health sleep analysis — 2026-07-06/07 night",
        "",
        "## Session summary",
        "",
        f"| Field | Value |",
        f"|-------|-------|",
        f"| sleep_id | `{session.sleep_id}` |",
        f"| bed (start) | {session.bed_time:%Y-%m-%d %H:%M} |",
        f"| wake (end) | {session.wake_time:%Y-%m-%d %H:%M} |",
        f"| time in bed | {fmt_minutes(in_bed_min)} |",
        f"| sleep duration (Samsung) | {session.duration_min} min ({fmt_minutes(session.duration_min)}) |",
        f"| sleep score | {session.sleep_score} |",
        f"| efficiency | {session.efficiency}% |",
        f"| sleep latency | {session.sleep_latency_min:.0f} min |" if session.sleep_latency_min else "| sleep latency | — |",
        f"| movement awakenings | {session.movement_awakening} |" if session.movement_awakening is not None else "",
        f"| mental recovery | {session.mental_recovery} |" if session.mental_recovery is not None else "",
        f"| physical recovery | {session.physical_recovery} |" if session.physical_recovery is not None else "",
        "",
        "## Stage breakdown (from sleep_stage segments)",
        "",
        f"| stage | minutes | % of in-bed |",
        f"|-------|---------|-------------|",
    ]
    for stage in ("awake", "light", "deep", "rem"):
        m = durations.get(stage, 0)
        pct = 100 * m / in_bed_min if in_bed_min else 0
        lines.append(f"| {stage} | {m:.1f} | {pct:.1f}% |")
    lines.append(f"| **asleep (L+D+R)** | **{asleep_min:.1f}** | **{100*asleep_min/in_bed_min:.1f}%** |")

    lines.extend(
        [
            "",
            "## Radar log window (test-sleep-net)",
            "",
            f"- radar capture: **{radar_t0:%Y-%m-%d %H:%M}** → **{radar_t1:%Y-%m-%d %H:%M}**",
            f"- Samsung session ends **{(radar_t0 - session.wake_time).total_seconds()/60:.0f} min before** radar starts",
            "",
            "Samsung 수면은 01:31에 종료되고, 레이더 로그는 03:43부터라 **본 수면 구간과 직접 겹치는 구간이 없습니다.**",
            "아래 비교는 레이더가 켜진 이후(재취침·낮잠·침대 체류 가능) 구간 기준입니다.",
            "",
            "### Radar status (valid rows)",
            "",
            "| model | absent | awake | asleep |",
            "|-------|--------|-------|--------|",
        ]
    )
    for key, rows in sorted(radar_rows.items()):
        ratio = radar_status_ratio(rows)
        lines.append(
            f"| {key} | {100*ratio.get('absent',0):.1f}% | {100*ratio.get('awake',0):.1f}% | {100*ratio.get('asleep',0):.1f}% |"
        )

  # overlap window if any
    overlap_t0 = max(session.bed_time, radar_t0)
    overlap_t1 = min(session.wake_time, radar_t1)
    if overlap_t1 > overlap_t0:
        samsung_asleep = asleep_ratio_from_stages(segments, overlap_t0, overlap_t1)
        lines.extend(
            [
                "",
                f"### Overlap {overlap_t0:%H:%M}–{overlap_t1:%H:%M}",
                f"- Samsung non-awake ratio in overlap: **{100*samsung_asleep:.1f}%**",
            ]
        )
        for key, rows in sorted(radar_rows.items()):
            sub = [r for r in rows if overlap_t0 <= r.ts < overlap_t1]
            ratio = radar_status_ratio(sub)
            lines.append(
                f"- radar {key} asleep: **{100*ratio.get('asleep',0):.1f}%**"
            )

    lines.extend(
        [
            "",
            "## Interpretation notes",
            "",
            "- Samsung은 손목(워치) 기준 수면 단계; 레이더는 침대/방 기준 absent·awake·asleep.",
            "- 08시 전후 레이더 0-0 CNN은 absent 급증(갭 민감) — 워치는 이미 01:31에 기상.",
            "- 레이더 03:43~11:11 구간은 워치 수면 세션 **이후**라, '같은 밤 수면'의 후반 tail이 아니라 **별 구간**일 가능성이 큼.",
            "- toss_index는 워치 움직임과 직접 대응하지 않음(레이더는 point cloud 기반).",
        ]
    )
    out_path.write_text("\n".join(line for line in lines if line is not None), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--export-dir",
        type=Path,
        default=Path(__file__).resolve().parent.parent
        / "test"
        / "samsunghealth_20260707"
        / "samsunghealth_j099450000_20260707194622",
    )
    parser.add_argument(
        "--sleep-id",
        default="4e4fb282-d4dd-4ab5-8329-83eb59fcfb93",
        help="July 6-7 overnight session UUID",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "test" / "plots" / "20260707" / "samsung",
    )
    args = parser.parse_args()

    sessions = load_sleep_sessions(args.export_dir)
    session = next((s for s in sessions if s.sleep_id == args.sleep_id), None)
    if session is None:
        raise SystemExit(f"sleep_id not found: {args.sleep_id}")

    segments = load_stage_segments(args.export_dir, session.sleep_id)
    durations = stage_durations(segments)

    test_dir = Path(__file__).resolve().parent.parent / "test"
    radar_rows: Dict[str, List[RadarRow]] = {}
    for suffix, key in [("-0-0", "0-0"), ("-0-1", "0-1"), ("-0-2", "0-2")]:
        path = test_dir / f"sleep_20260707{suffix}.csv"
        if path.exists():
            radar_rows[key] = load_radar_csv(path)

    args.output.mkdir(parents=True, exist_ok=True)
    plot_stage_timeline(segments, session, args.output / "samsung_stage_timeline.png")
    plot_stage_pie(durations, args.output / "samsung_stage_pie.png")
    if radar_rows:
        t0 = min(r.ts for rows in radar_rows.values() for r in rows)
        t1 = max(r.ts for rows in radar_rows.values() for r in rows)
        plot_comparison(segments, radar_rows, (t0, t1), args.output / "samsung_vs_radar.png")
    write_summary(session, segments, durations, radar_rows, args.output / "summary.md")

    print(f"Session: {session.bed_time} -> {session.wake_time}")
    print(f"Score {session.sleep_score}, efficiency {session.efficiency}%")
    for stage in ("awake", "light", "deep", "rem"):
        print(f"  {stage}: {durations.get(stage, 0):.1f} min")
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
