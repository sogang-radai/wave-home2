#!/usr/bin/env python3
"""Plot sleep-net CSV logs (multi-model comparison)."""

from __future__ import annotations

import argparse
import csv
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch

STATUS_ORDER = ("absent", "awake", "asleep", "?")
STATUS_TO_Y = {"absent": 0.0, "awake": 1.0, "asleep": 2.0, "?": np.nan}
STATUS_COLORS = {
    "absent": "#e15759",
    "awake": "#f28e2b",
    "asleep": "#4e79a7",
}


@dataclass
class Row:
    ts: datetime
    status: str
    toss_index: float
    toss_valid: bool
    frame_gap: bool
    warmup: bool
    connected: bool
    frame_begin: int
    frame_end: int


def parse_csv(path: Path) -> List[Row]:
    rows: List[Row] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for raw in csv.DictReader(handle):
            ts = datetime.strptime(f"{raw['date']} {raw['time']}", "%Y-%m-%d %H:%M:%S")
            toss_index = float(raw["toss_index"]) if raw.get("toss_index") else np.nan
            rows.append(
                Row(
                    ts=ts,
                    status=raw.get("status_label") or "?",
                    toss_index=toss_index,
                    toss_valid=raw.get("toss_valid") == "1",
                    frame_gap=raw.get("frame_gap") == "1",
                    warmup=raw.get("warmup") == "1",
                    connected=raw.get("connected") == "1",
                    frame_begin=int(raw["frame_begin"] or 0),
                    frame_end=int(raw["frame_end"] or 0),
                )
            )
    return rows


def valid_rows(rows: Sequence[Row]) -> List[Row]:
    return [r for r in rows if not r.warmup and r.connected]


def downsample_idx(n: int, max_points: int = 4000) -> np.ndarray:
    if n <= max_points:
        return np.arange(n)
    return np.unique(np.linspace(0, n - 1, max_points, dtype=int))


def shade_gaps(ax, times: Sequence[datetime], gaps: Sequence[bool], *, alpha: float = 0.12) -> None:
    start: Optional[datetime] = None
    for ts, is_gap in zip(times, gaps):
        if is_gap and start is None:
            start = ts
        elif not is_gap and start is not None:
            ax.axvspan(start, ts, color="#888888", alpha=alpha, linewidth=0)
            start = None
    if start is not None and times:
        ax.axvspan(start, times[-1], color="#888888", alpha=alpha, linewidth=0)


MODEL_STYLES = {
    "0-0": {"color": "#59a14f", "label": "0-0 CNN bed160"},
    "0-1": {"color": "#edc948", "label": "0-1 LSTM bed160"},
    "0-2": {"color": "#b07aa1", "label": "0-2 LSTM bed100"},
}


def plot_single_model(key: str, rows: List[Row], out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    style = MODEL_STYLES.get(key, {"color": "#333333", "label": key})
    color = style["color"]
    label = style["label"]
    data = valid_rows(rows)
    if not data:
        return

    idx = downsample_idx(len(data))
    times = [data[i].ts for i in idx]
    status_y = [STATUS_TO_Y.get(data[i].status, np.nan) for i in idx]
    toss = [data[i].toss_index if data[i].toss_valid else np.nan for i in idx]
    gaps = [data[i].frame_gap for i in idx]

    fig, axes = plt.subplots(3, 1, figsize=(16, 9), sharex=True, constrained_layout=True)
    fig.suptitle(f"SleepNet overnight — {label}", fontsize=14, fontweight="bold")

    shade_gaps(axes[0], times, gaps)
    axes[0].plot(times, status_y, lw=1.1, color=color, alpha=0.95)
    axes[0].set_yticks([0, 1, 2])
    axes[0].set_yticklabels(["absent", "awake", "asleep"])
    axes[0].set_ylabel("status")
    axes[0].grid(True, alpha=0.25)

    axes[1].plot(times, toss, lw=0.9, color=color, alpha=0.9)
    axes[1].set_ylabel("toss_index")
    axes[1].set_ylim(-0.05, 1.05)
    axes[1].grid(True, alpha=0.25)

    axes[2].fill_between(times, 0, [1.0 if g else 0.0 for g in gaps], step="post", color="#888888", alpha=0.55)
    axes[2].set_yticks([0, 1])
    axes[2].set_yticklabels(["ok", "gap"])
    axes[2].set_ylabel("frame_gap")
    axes[2].set_xlabel("time")
    axes[2].grid(True, alpha=0.2)
    for ax in axes:
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.savefig(out_dir / "timeline.png", dpi=150)
    plt.close(fig)

    # status colored scatter (full valid series, downsampled)
    fig, ax = plt.subplots(figsize=(16, 3.5), constrained_layout=True)
    fig.suptitle(f"Status timeline — {label}", fontsize=12, fontweight="bold")
    for status, y in STATUS_TO_Y.items():
        if status == "?":
            continue
        pts = [data[i].ts for i in idx if data[i].status == status]
        ys = [y] * len(pts)
        ax.scatter(pts, ys, s=8, c=STATUS_COLORS.get(status, "#999"), alpha=0.7, label=status)
    shade_gaps(ax, times, gaps, alpha=0.1)
    ax.set_yticks([0, 1, 2])
    ax.set_yticklabels(["absent", "awake", "asleep"])
    ax.legend(loc="upper right", ncol=3, fontsize=9)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax.grid(True, alpha=0.2)
    fig.savefig(out_dir / "status_scatter.png", dpi=150)
    plt.close(fig)

    asleep_toss = [
        r.toss_index
        for r in data
        if r.status == "asleep" and r.toss_valid and not np.isnan(r.toss_index)
    ]
    if asleep_toss:
        fig, axes = plt.subplots(1, 2, figsize=(12, 4), constrained_layout=True)
        fig.suptitle(f"Toss index (asleep) — {label}", fontsize=12, fontweight="bold")
        axes[0].hist(asleep_toss, bins=50, range=(0, 1), color=color, alpha=0.75, density=True)
        axes[0].set_xlabel("toss_index")
        axes[0].grid(True, alpha=0.25)
        sorted_vals = np.sort(asleep_toss)
        axes[1].plot(sorted_vals, np.linspace(0, 1, len(sorted_vals)), color=color, lw=1.5)
        axes[1].set_xlabel("toss_index")
        axes[1].set_ylabel("CDF")
        axes[1].grid(True, alpha=0.25)
        fig.savefig(out_dir / "toss_distribution.png", dpi=150)
        plt.close(fig)

    hours = [f"{h:02d}" for h in range(3, 12)]
    hourly = {h: Counter() for h in hours}
    for r in data:
        h = r.ts.strftime("%H")
        if h in hourly and r.status in STATUS_ORDER:
            hourly[h][r.status] += 1

    fig, ax = plt.subplots(figsize=(12, 4), constrained_layout=True)
    fig.suptitle(f"Hourly status — {label}", fontsize=12, fontweight="bold")
    x = np.arange(len(hours))
    width = 0.55
    asleep = []
    awake = []
    absent = []
    for h in hours:
        total = sum(hourly[h].values()) or 1
        absent.append(100 * hourly[h]["absent"] / total)
        awake.append(100 * hourly[h]["awake"] / total)
        asleep.append(100 * hourly[h]["asleep"] / total)
    ax.bar(x, asleep, width, label="asleep", color=STATUS_COLORS["asleep"], alpha=0.9)
    ax.bar(x, awake, width, bottom=asleep, label="awake", color=STATUS_COLORS["awake"], alpha=0.9)
    bottom = np.array(asleep) + np.array(awake)
    ax.bar(x, absent, width, bottom=bottom, label="absent", color=STATUS_COLORS["absent"], alpha=0.9)
    ax.set_xticks(x)
    ax.set_xticklabels([f"{h}:00" for h in hours])
    ax.set_ylabel("percent")
    ax.set_ylim(0, 105)
    ax.legend(loc="upper right")
    ax.grid(True, axis="y", alpha=0.25)
    fig.savefig(out_dir / "hourly_status.png", dpi=150)
    plt.close(fig)

    t0 = datetime(2026, 7, 7, 8, 0, 0)
    t1 = datetime(2026, 7, 7, 9, 0, 0)
    window = [r for r in data if t0 <= r.ts < t1]
    if window:
        fig, axes = plt.subplots(2, 1, figsize=(14, 6), sharex=True, constrained_layout=True)
        fig.suptitle(f"08:00–09:00 — {label}", fontsize=12, fontweight="bold")
        w_times = [r.ts for r in window]
        axes[0].step(w_times, [STATUS_TO_Y.get(r.status, np.nan) for r in window], where="post", color=color)
        axes[1].plot(w_times, [r.toss_index if r.toss_valid else np.nan for r in window], color=color, lw=1)
        shade_gaps(axes[0], w_times, [r.frame_gap for r in window], alpha=0.15)
        axes[0].set_yticks([0, 1, 2])
        axes[0].set_yticklabels(["absent", "awake", "asleep"])
        axes[0].grid(True, alpha=0.25)
        axes[1].set_ylim(-0.05, 1.05)
        axes[1].grid(True, alpha=0.25)
        axes[1].set_xlabel("time")
        for ax in axes:
            ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
        fig.savefig(out_dir / "zoom_08.png", dpi=150)
        plt.close(fig)


def plot_per_model(series: Dict[str, List[Row]], base_out_dir: Path) -> None:
    for key, rows in series.items():
        model_dir = base_out_dir / key
        plot_single_model(key, rows, model_dir)
        print(f"  {key}/")


def plot_overview(
    series: Dict[str, List[Row]],
    out_dir: Path,
) -> None:
    fig, axes = plt.subplots(3, 1, figsize=(16, 10), sharex=True, constrained_layout=True)
    fig.suptitle("SleepNet overnight comparison (2026-07-07)", fontsize=14, fontweight="bold")

    model_styles = MODEL_STYLES

    ref_key = next(iter(series))
    ref = valid_rows(series[ref_key])
    idx = downsample_idx(len(ref))
    ref_times = [ref[i].ts for i in idx]
    shade_gaps(axes[0], ref_times, [ref[i].frame_gap for i in idx])

    for key, rows in series.items():
        data = valid_rows(rows)
        style = model_styles.get(key, {"color": None, "label": key})
        times = [data[i].ts for i in idx if i < len(data)]
        status_y = [STATUS_TO_Y.get(data[i].status, np.nan) for i in idx if i < len(data)]
        toss = [data[i].toss_index if data[i].toss_valid else np.nan for i in idx if i < len(data)]

        axes[0].plot(times, status_y, lw=1.0, alpha=0.9, **style)
        axes[1].plot(times, toss, lw=0.9, alpha=0.85, **style)

    axes[0].set_yticks([0, 1, 2])
    axes[0].set_yticklabels(["absent", "awake", "asleep"])
    axes[0].set_ylabel("status")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right", fontsize=9)

    axes[1].set_ylabel("toss_index")
    axes[1].set_ylim(-0.05, 1.05)
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right", fontsize=9)

    disagree = []
    keys = list(series.keys())
    if len(keys) >= 3:
        maps = [{r.ts: r for r in valid_rows(series[k])} for k in keys]
        common_ts = sorted(set(maps[0]) & set(maps[1]) & set(maps[2]))
        for ts in common_ts:
            labels = [m[ts].status for m in maps]
            if len(set(labels)) > 1:
                disagree.append((ts, labels))

    if disagree:
        d_times = [d[0] for d in disagree]
        axes[2].scatter(d_times, np.ones(len(d_times)), s=6, c="#d62728", alpha=0.5, label="status disagree")
    axes[2].set_yticks([])
    axes[2].set_ylabel("disagree")
    axes[2].set_xlabel("time")
    axes[2].legend(loc="upper right", fontsize=9)

    for ax in axes:
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.savefig(out_dir / "overview_timeline.png", dpi=150)
    plt.close(fig)


def plot_zoom_08(
    series: Dict[str, List[Row]],
    out_dir: Path,
) -> None:
    fig, axes = plt.subplots(2, 1, figsize=(16, 7), sharex=True, constrained_layout=True)
    fig.suptitle("08:00–09:00 zoom (0-0 absent spike vs LSTM)", fontsize=13, fontweight="bold")

    t0 = datetime(2026, 7, 7, 8, 0, 0)
    t1 = datetime(2026, 7, 7, 9, 0, 0)
    styles = {
        "0-0": ("#59a14f", "0-0 CNN"),
        "0-1": ("#edc948", "0-1 LSTM"),
        "0-2": ("#b07aa1", "0-2 LSTM"),
    }

    for key, rows in series.items():
        window = [r for r in valid_rows(rows) if t0 <= r.ts < t1]
        if not window:
            continue
        color, label = styles[key]
        times = [r.ts for r in window]
        status_y = [STATUS_TO_Y.get(r.status, np.nan) for r in window]
        toss = [r.toss_index if r.toss_valid else np.nan for r in window]
        axes[0].step(times, status_y, where="post", lw=1.2, color=color, label=label, alpha=0.9)
        axes[1].plot(times, toss, lw=1.0, color=color, alpha=0.85, label=label)

    ref = [r for r in valid_rows(series["0-0"]) if t0 <= r.ts < t1]
    shade_gaps(axes[0], [r.ts for r in ref], [r.frame_gap for r in ref], alpha=0.18)

    axes[0].set_yticks([0, 1, 2])
    axes[0].set_yticklabels(["absent", "awake", "asleep"])
    axes[0].set_ylabel("status")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].set_ylabel("toss_index")
    axes[1].set_ylim(-0.05, 1.05)
    axes[1].grid(True, alpha=0.25)
    axes[1].legend(loc="upper right")
    axes[1].set_xlabel("time")
    for ax in axes:
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    fig.savefig(out_dir / "zoom_08_status_toss.png", dpi=150)
    plt.close(fig)


def plot_toss_distributions(series: Dict[str, List[Row]], out_dir: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), constrained_layout=True)
    fig.suptitle("Toss index distribution (asleep rows only)", fontsize=13, fontweight="bold")

    colors = {"0-0": "#59a14f", "0-1": "#edc948", "0-2": "#b07aa1"}
    for key, rows in series.items():
        asleep_toss = [
            r.toss_index
            for r in valid_rows(rows)
            if r.status == "asleep" and r.toss_valid and not np.isnan(r.toss_index)
        ]
        if not asleep_toss:
            continue
        axes[0].hist(
            asleep_toss,
            bins=50,
            range=(0.0, 1.0),
            alpha=0.45,
            color=colors.get(key, "#333333"),
            label=f"{key} (n={len(asleep_toss)})",
            density=True,
        )
        sorted_vals = np.sort(asleep_toss)
        cdf_y = np.linspace(0, 1, len(sorted_vals), endpoint=True)
        axes[1].plot(sorted_vals, cdf_y, lw=1.5, color=colors.get(key, "#333333"), label=key)

    axes[0].set_xlabel("toss_index")
    axes[0].set_ylabel("density")
    axes[0].legend(fontsize=9)
    axes[0].grid(True, alpha=0.25)

    axes[1].set_xlabel("toss_index")
    axes[1].set_ylabel("CDF")
    axes[1].legend(fontsize=9)
    axes[1].grid(True, alpha=0.25)

    fig.savefig(out_dir / "toss_distribution_asleep.png", dpi=150)
    plt.close(fig)


def plot_status_hours(series: Dict[str, List[Row]], out_dir: Path) -> None:
    fig, ax = plt.subplots(figsize=(14, 5), constrained_layout=True)
    fig.suptitle("Hourly status composition", fontsize=13, fontweight="bold")

    hours = [f"{h:02d}" for h in range(3, 12)]
    x = np.arange(len(hours))
    width = 0.25
    colors = {"0-0": "#59a14f", "0-1": "#edc948", "0-2": "#b07aa1"}

    for offset, (key, rows) in enumerate(series.items()):
        hourly: Dict[str, Dict[str, int]] = {h: {"absent": 0, "awake": 0, "asleep": 0} for h in hours}
        for r in valid_rows(rows):
            hour = r.ts.strftime("%H")
            if hour not in hourly or r.status not in hourly[hour]:
                continue
            hourly[hour][r.status] += 1

        asleep_pct = []
        absent_pct = []
        for h in hours:
            total = sum(hourly[h].values())
            asleep_pct.append(100.0 * hourly[h]["asleep"] / total if total else 0.0)
            absent_pct.append(100.0 * hourly[h]["absent"] / total if total else 0.0)

        pos = x + (offset - 1) * width
        ax.bar(pos, asleep_pct, width=width, color=colors[key], alpha=0.85, label=f"{key} asleep")
        ax.bar(pos, absent_pct, width=width, bottom=asleep_pct, color=colors[key], alpha=0.35,
               hatch="//", label=f"{key} absent" if offset == 0 else None)

    ax.set_xticks(x)
    ax.set_xticklabels([f"{h}:00" for h in hours])
    ax.set_ylabel("percent")
    ax.set_ylim(0, 105)
    ax.grid(True, axis="y", alpha=0.25)
    ax.legend(ncol=3, fontsize=8, loc="upper right")
    fig.savefig(out_dir / "hourly_status.png", dpi=150)
    plt.close(fig)


def plot_gap_timeline(series: Dict[str, List[Row]], out_dir: Path) -> None:
    ref = valid_rows(series[next(iter(series))])
    gaps = np.array([1.0 if r.frame_gap else 0.0 for r in ref], dtype=float)

    fig, ax = plt.subplots(figsize=(16, 2.8), constrained_layout=True)
    fig.suptitle("Frame gap timeline (shared across models)", fontsize=12, fontweight="bold")
    times = [r.ts for r in ref]
    ax.fill_between(times, 0, gaps, step="post", color="#888888", alpha=0.6)
    ax.set_ylim(-0.05, 1.2)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["ok", "gap"])
    ax.set_xlabel("time")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax.grid(True, alpha=0.2)
    fig.savefig(out_dir / "frame_gap_timeline.png", dpi=150)
    plt.close(fig)


def write_summary(series: Dict[str, List[Row]], out_dir: Path) -> None:
    lines = ["# Sleep log analysis summary", ""]
    for key, rows in series.items():
        data = valid_rows(rows)
        n = len(data)
        status_counts = {s: sum(1 for r in data if r.status == s) for s in STATUS_ORDER[:3]}
        gap_n = sum(1 for r in data if r.frame_gap)
        toss = [r.toss_index for r in data if r.status == "asleep" and r.toss_valid]
        lines.append(f"## {key}")
        lines.append(f"- rows(valid): {n}")
        lines.append(f"- frame_gap rows: {gap_n} ({100*gap_n/max(n,1):.1f}%)")
        for s in STATUS_ORDER[:3]:
            lines.append(f"- {s}: {status_counts.get(s,0)} ({100*status_counts.get(s,0)/max(n,1):.1f}%)")
        if toss:
            lines.append(
                f"- toss_index(asleep): mean={np.mean(toss):.3f} p50={np.median(toss):.3f} "
                f"p90={np.quantile(toss,0.9):.3f}"
            )
        lines.append("")
    (out_dir / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot sleep-net CSV logs.")
    parser.add_argument(
        "csvs",
        nargs="*",
        type=Path,
        help="CSV paths (default: sleep-net/test/sleep_20260707-0-*.csv)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "test" / "plots" / "20260707",
    )
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    default_dir = script_dir.parent / "test"
    if not args.csvs:
        paths = sorted(default_dir.glob("sleep_20260707-0-*.csv"))
    else:
        paths = args.csvs

    if not paths:
        raise SystemExit("no CSV files found")

    series: Dict[str, List[Row]] = {}
    for path in paths:
        stem = path.stem
        if stem.endswith("-0-2"):
            key = "0-2"
        elif stem.endswith("-0-1"):
            key = "0-1"
        elif stem.endswith("-0-0"):
            key = "0-0"
        else:
            key = stem.split("_")[-1]
        series[key] = parse_csv(path)

    args.output.mkdir(parents=True, exist_ok=True)
    plot_overview(series, args.output)
    plot_zoom_08(series, args.output)
    plot_toss_distributions(series, args.output)
    plot_status_hours(series, args.output)
    plot_gap_timeline(series, args.output)
    write_summary(series, args.output)
    print("Per-model plots:")
    plot_per_model(series, args.output)

    print(f"\nWrote plots to {args.output}")
    for name in sorted(args.output.glob("*.png")):
        print(f"  {name.name}")
    for sub in sorted(args.output.iterdir()):
        if sub.is_dir():
            for name in sorted(sub.glob("*.png")):
                print(f"  {sub.name}/{name.name}")


if __name__ == "__main__":
    main()
