"""Analyze an overnight SleepNet CSV log produced by test-sleep-net.

The CSV is written one row per aggregation window (default ~1 s). Columns:
    date, time, frame_begin, frame_end, samples,
    status_label, status_absent, status_awake, status_asleep,
    toss_valid, toss_label, toss_index, toss_calm, toss_slight, toss_moderate

Usage:
    ./.venv/bin/python analyze_sleep_log.py sleep_20260703.csv
    ./.venv/bin/python analyze_sleep_log.py sleep_20260703.csv --no-plot
    ./.venv/bin/python analyze_sleep_log.py sleep_20260703.csv --out report/

Outputs:
    - Text summary to stdout (durations, sleep window, toss statistics, episodes).
    - <out>/summary.txt   : same text summary saved to file.
    - <out>/timeline.png  : status timeline + toss-index over the night (unless --no-plot).
    - <out>/hourly.csv    : per-hour aggregates.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd


STATUS_LABELS = ["absent", "awake", "asleep"]
TOSS_LABELS = ["calm", "slight", "moderate"]
STATUS_CODE = {"absent": 0, "awake": 1, "asleep": 2}


def load(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)

    df["timestamp"] = pd.to_datetime(
        df["date"].astype(str) + " " + df["time"].astype(str),
        format="%Y-%m-%d %H:%M:%S",
    )
    df = df.sort_values("timestamp").reset_index(drop=True)

    # Each row covers roughly the time until the next row; use that as its weight
    # so gaps (e.g. warmup, reconnects) don't get counted as fixed 1 s.
    delta = df["timestamp"].diff().dt.total_seconds().shift(-1)
    median_dt = delta.median()
    if not np.isfinite(median_dt) or median_dt <= 0:
        median_dt = 1.0
    # Clamp gaps: a jump larger than 5x the median cadence is treated as a gap
    # of one median step (the sensor was not producing continuous windows).
    delta = delta.fillna(median_dt).clip(upper=median_dt * 5)
    df["duration_s"] = delta

    return df


def fmt_hms(seconds: float) -> str:
    seconds = int(round(seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h:d}h {m:02d}m {s:02d}s"


def status_durations(df: pd.DataFrame) -> pd.Series:
    return df.groupby("status_label")["duration_s"].sum()


def coverage_stats(df: pd.DataFrame, gap_threshold: float = 5.0) -> dict:
    """Gaps = periods with no inference output (frame drops / broken windows)."""
    diff = df["timestamp"].diff().dt.total_seconds().dropna()
    gaps = diff[diff > gap_threshold]
    return {
        "gap_count": int(gaps.count()),
        "gap_total_s": float(gaps.sum()),
        "max_gap_s": float(gaps.max()) if not gaps.empty else 0.0,
        "threshold": gap_threshold,
    }


def find_episodes(df: pd.DataFrame, min_seconds: float = 30.0) -> pd.DataFrame:
    """Merge consecutive rows with the same status into episodes."""
    if df.empty:
        return pd.DataFrame(columns=["status", "start", "end", "duration_s"])

    change = (df["status_label"] != df["status_label"].shift()).cumsum()
    rows = []
    for _, group in df.groupby(change):
        rows.append(
            {
                "status": group["status_label"].iloc[0],
                "start": group["timestamp"].iloc[0],
                "end": group["timestamp"].iloc[-1],
                "duration_s": group["duration_s"].sum(),
            }
        )
    episodes = pd.DataFrame(rows)
    return episodes[episodes["duration_s"] >= min_seconds].reset_index(drop=True)


def sleep_window(df: pd.DataFrame):
    asleep = df[df["status_label"] == "asleep"]
    if asleep.empty:
        return None, None
    return asleep["timestamp"].iloc[0], asleep["timestamp"].iloc[-1]


def toss_stats(df: pd.DataFrame) -> dict:
    valid = df[df["toss_valid"] == 1].copy()
    if valid.empty:
        return {}

    idx = valid["toss_index"].astype(float)
    label_dur = valid.groupby("toss_label")["duration_s"].sum()

    # A "movement event": toss_index crosses above a threshold.
    threshold = 0.5
    above = idx.to_numpy() >= threshold
    events = int(np.sum(np.diff(above.astype(int)) == 1) + (1 if above[:1].any() else 0))

    return {
        "valid_rows": len(valid),
        "mean_index": float(idx.mean()),
        "median_index": float(idx.median()),
        "p90_index": float(idx.quantile(0.90)),
        "max_index": float(idx.max()),
        "label_duration": label_dur,
        "movement_events": events,
        "threshold": threshold,
    }


def hourly_table(df: pd.DataFrame) -> pd.DataFrame:
    g = df.copy()
    g["hour"] = g["timestamp"].dt.floor("h")

    rows = []
    for hour, group in g.groupby("hour"):
        durs = group.groupby("status_label")["duration_s"].sum()
        valid = group[group["toss_valid"] == 1]
        rows.append(
            {
                "hour": hour,
                "absent_min": durs.get("absent", 0.0) / 60.0,
                "awake_min": durs.get("awake", 0.0) / 60.0,
                "asleep_min": durs.get("asleep", 0.0) / 60.0,
                "toss_mean": float(valid["toss_index"].astype(float).mean()) if not valid.empty else np.nan,
                "toss_max": float(valid["toss_index"].astype(float).max()) if not valid.empty else np.nan,
            }
        )
    return pd.DataFrame(rows)


def build_summary(df: pd.DataFrame) -> str:
    lines = []
    start = df["timestamp"].iloc[0]
    end = df["timestamp"].iloc[-1]
    total = df["duration_s"].sum()

    lines.append("=" * 60)
    lines.append("SleepNet overnight log analysis")
    lines.append("=" * 60)
    lines.append(f"rows           : {len(df)}")
    lines.append(f"recording start: {start}")
    lines.append(f"recording end  : {end}")
    lines.append(f"covered time   : {fmt_hms(total)}  (wall {fmt_hms((end - start).total_seconds())})")
    lines.append("")

    cov = coverage_stats(df)
    if cov["gap_count"] > 0:
        lines.append("--- data coverage ---")
        lines.append(
            f"  output gaps (> {cov['threshold']:.0f}s): {cov['gap_count']}  "
            f"total {fmt_hms(cov['gap_total_s'])}  max {fmt_hms(cov['max_gap_s'])}"
        )
        lines.append("  (gaps = no inference output: radar frame drops or broken 160-frame window)")
        lines.append("")

    lines.append("--- status durations ---")
    durs = status_durations(df)
    for label in STATUS_LABELS:
        sec = float(durs.get(label, 0.0))
        pct = 100.0 * sec / total if total > 0 else 0.0
        lines.append(f"  {label:7s}: {fmt_hms(sec):>14s}  ({pct:5.1f}%)")
    lines.append("")

    s_start, s_end = sleep_window(df)
    if s_start is not None:
        asleep_sec = float(durs.get("asleep", 0.0))
        in_bed = (s_end - s_start).total_seconds()
        eff = 100.0 * asleep_sec / in_bed if in_bed > 0 else 0.0
        lines.append("--- sleep window (first to last 'asleep') ---")
        lines.append(f"  onset (sleep start): {s_start}")
        lines.append(f"  final wake         : {s_end}")
        lines.append(f"  time in bed        : {fmt_hms(in_bed)}")
        lines.append(f"  asleep total       : {fmt_hms(asleep_sec)}")
        lines.append(f"  sleep efficiency   : {eff:5.1f}%  (asleep / time-in-bed)")
        lines.append("")

    ts = toss_stats(df)
    if ts:
        lines.append("--- toss (movement) while asleep ---")
        lines.append(f"  valid rows     : {ts['valid_rows']}")
        lines.append(f"  index mean     : {ts['mean_index']:.3f}")
        lines.append(f"  index median   : {ts['median_index']:.3f}")
        lines.append(f"  index p90      : {ts['p90_index']:.3f}")
        lines.append(f"  index max      : {ts['max_index']:.3f}")
        lines.append(f"  movement events: {ts['movement_events']}  (index >= {ts['threshold']})")
        lines.append("  toss label durations:")
        for label in TOSS_LABELS:
            sec = float(ts["label_duration"].get(label, 0.0))
            lines.append(f"    {label:8s}: {fmt_hms(sec)}")
        lines.append("")

    episodes = find_episodes(df, min_seconds=30.0)
    if not episodes.empty:
        lines.append("--- episodes (>= 30 s, merged) ---")
        for _, ep in episodes.iterrows():
            lines.append(
                f"  {ep['start'].strftime('%H:%M:%S')} - {ep['end'].strftime('%H:%M:%S')}  "
                f"{ep['status']:7s}  {fmt_hms(ep['duration_s'])}"
            )
        lines.append("")

    return "\n".join(lines)


def make_plot(df: pd.DataFrame, out_path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    status_code = df["status_label"].map(STATUS_CODE).astype(float)
    toss = df["toss_index"].where(df["toss_valid"] == 1).astype(float)

    fig, (ax_status, ax_toss) = plt.subplots(
        2, 1, figsize=(14, 6), sharex=True, gridspec_kw={"height_ratios": [1, 1]}
    )

    ax_status.step(df["timestamp"], status_code, where="post", color="#3366cc", linewidth=1.0)
    ax_status.set_yticks([0, 1, 2])
    ax_status.set_yticklabels(STATUS_LABELS)
    ax_status.set_ylim(-0.3, 2.3)
    ax_status.set_title("Sleep status over the night")
    ax_status.grid(True, axis="x", alpha=0.3)

    ax_toss.plot(df["timestamp"], toss, color="#cc4444", linewidth=0.7)
    ax_toss.axhline(0.5, color="gray", linestyle="--", linewidth=0.8, label="movement threshold")
    ax_toss.set_ylim(0, 1.05)
    ax_toss.set_ylabel("toss index")
    ax_toss.set_title("Toss (movement) index while asleep")
    ax_toss.grid(True, alpha=0.3)
    ax_toss.legend(loc="upper right")

    ax_toss.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax_toss.xaxis.set_major_locator(mdates.AutoDateLocator())
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def main() -> None:
    ap = argparse.ArgumentParser(description="Analyze a SleepNet overnight CSV log.")
    ap.add_argument("csv", type=Path, help="CSV log path.")
    ap.add_argument("--out", type=Path, default=None, help="output directory (default: <csv>_analysis).")
    ap.add_argument("--no-plot", action="store_true", help="skip PNG plot generation.")
    args = ap.parse_args()

    if not args.csv.exists():
        raise SystemExit(f"file not found: {args.csv}")

    out_dir = args.out or args.csv.with_name(args.csv.stem + "_analysis")
    out_dir.mkdir(parents=True, exist_ok=True)

    df = load(args.csv)
    summary = build_summary(df)
    print(summary)

    (out_dir / "summary.txt").write_text(summary + "\n", encoding="utf-8")
    hourly_table(df).to_csv(out_dir / "hourly.csv", index=False)

    if not args.no_plot:
        make_plot(df, out_dir / "timeline.png")
        print(f"\nplot saved  : {out_dir / 'timeline.png'}")

    print(f"summary saved: {out_dir / 'summary.txt'}")
    print(f"hourly saved : {out_dir / 'hourly.csv'}")


if __name__ == "__main__":
    main()
