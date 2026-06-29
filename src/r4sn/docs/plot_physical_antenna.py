#!/usr/bin/env python3
"""Plot and validate physical antenna layout (hardcoded coordinates, no file parsing)."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

DOCS = Path(__file__).resolve().parent

# --- constants (physical-antenna.txt header) ---
L_MM = 3.893408545
D_MM = 1.946704273
Q_MM = 0.973352136

C = 299_792_458.0
FC_HZ = 77.0e9
TOL_D = 1e-9

# --- coordinates in d = λ/2 units, origin S1-TX1, +x right, +y up ---
# fmt: (x_d, y_d) — must match docs/physical-antenna.txt
PHYSICAL: dict[str, dict[str, tuple[float, float]]] = {
    "S1": {
        "TX1": (0, 0),
        "TX2": (1, -2),
        "TX3": (2, -3),
        "RX1": (1, 16),
        "RX2": (2, 17),
        "RX3": (3, 19),
        "RX4": (4, 20),
    },
    "S2": {
        "TX1": (-7, -10),
        "TX2": (-6, -11),
        "TX3": (-5, -13),
        "RX1": (0, 30),
        "RX2": (1, 30),
        "RX3": (3, 30),
        "RX4": (5, 30),
    },
    "S3": {
        "TX1": (3, -5),
        "TX2": (5, -6),
        "TX3": (6, -7),
        "RX1": (5, 21),
        "RX2": (6, 22),
        "RX3": (7, 22),
        "RX4": (9, 22),
    },
    "S4": {
        "TX1": (-10, -6),
        "TX2": (-9, -7),
        "TX3": (-8, -8),
        "RX1": (-5, 24),
        "RX2": (-4, 26),
        "RX3": (-3, 28),
        "RX4": (-1, 29),
    },
}

COLORS = {"S1": "#e53935", "S2": "#1e88e5", "S3": "#43a047", "S4": "#fb8c00"}
MODULE_ORDER = ("S1", "S2", "S3", "S4")


@dataclass(frozen=True)
class Antenna:
    submodule: str
    role: str
    index: int
    x_d: float
    y_d: float

    @property
    def label(self) -> str:
        return f"{self.submodule}-{self.role}{self.index}"


def build_antennas() -> dict[str, list[Antenna]]:
    modules: dict[str, list[Antenna]] = {}
    for mod in MODULE_ORDER:
        entries = PHYSICAL[mod]
        ants: list[Antenna] = []
        for key, (x, y) in entries.items():
            role, idx_s = key[:2], key[2:]
            ants.append(Antenna(mod, role, int(idx_s), float(x), float(y)))
        modules[mod] = sorted(ants, key=lambda a: (a.role, a.index))
    return modules


def build_lookup(modules: dict[str, list[Antenna]]) -> dict[str, tuple[float, float]]:
    return {a.label: (a.x_d, a.y_d) for ants in modules.values() for a in ants}


def tile_sub_to_labels(tile: int, sub_ant: int) -> tuple[str, str]:
    s = tile // 3 + 1
    tx = tile % 3 + 1
    rx_s = sub_ant // 4 + 1
    rx = sub_ant % 4 + 1
    return f"S{s}-TX{tx}", f"S{rx_s}-RX{rx}"


def compute_va_grid(lookup: dict[str, tuple[float, float]], d_mm: float) -> np.ndarray:
    pts: list[list[float]] = []
    for tile in range(12):
        for sub in range(16):
            tx_l, rx_l = tile_sub_to_labels(tile, sub)
            tx = np.array(lookup[tx_l])
            rx = np.array(lookup[rx_l])
            pts.append(((tx + rx) * d_mm).tolist())
    return np.array(pts)


def is_half_lambda_grid(v: float) -> bool:
    return abs(v * 2 - round(v * 2)) < TOL_D


def print_table(modules: dict[str, list[Antenna]]) -> None:
    print("\n[0] Hardcoded coordinates (d units)")
    for mod in MODULE_ORDER:
        print(f"  {mod}:")
        for a in modules[mod]:
            print(f"    {a.label:8s}  ({a.x_d:+.0f}d, {a.y_d:+.0f}d)")


def main() -> None:
    modules = build_antennas()
    lookup = build_lookup(modules)
    va_mm = compute_va_grid(lookup, D_MM)
    lam_theory = C / FC_HZ * 1e3

    print("=" * 60)
    print("physical-antenna 검증 (하드코딩 좌표)")
    print("=" * 60)
    print_table(modules)

    print("\n[1] 파장 / 단위")
    print(f"  l = {L_MM:.6f} mm, 77 GHz theory = {lam_theory:.6f} mm")
    print(f"  d/l/2 = {D_MM / (L_MM / 2):.6f},  q/l/4 = {Q_MM / (L_MM / 4):.6f}")
    ok_units = abs(D_MM / (L_MM / 2) - 1) < 1e-6 and abs(Q_MM / (L_MM / 4) - 1) < 1e-6
    print(f"  → {'PASS' if ok_units else 'FAIL'}")

    print("\n[2] λ/4 격자 (0.5d 배수)")
    bad = [
        f"  {a.label}: ({a.x_d}, {a.y_d})"
        for mod in MODULE_ORDER
        for a in modules[mod]
        if not is_half_lambda_grid(a.x_d) or not is_half_lambda_grid(a.y_d)
    ]
    print("  PASS" if not bad else "  FAIL:\n" + "\n".join(bad))

    print("\n[3] TX 간격")
    for mod in MODULE_ORDER:
        txs = [a for a in modules[mod] if a.role == "TX"]
        for i in range(len(txs) - 1):
            a, b = txs[i], txs[i + 1]
            dx, dy = b.x_d - a.x_d, b.y_d - a.y_d
            print(f"  {a.label}→{b.label}: Δ=({dx:+.0f}d, {dy:+.0f}d)  |Δ|={math.hypot(dx, dy):.3f}d")

    print("\n[4] RX 배열")
    for mod in MODULE_ORDER:
        rxs = [a for a in modules[mod] if a.role == "RX"]
        ys = [a.y_d for a in rxs]
        xs = [a.x_d for a in rxs]
        dxs = [xs[i + 1] - xs[i] for i in range(len(xs) - 1)]
        y_spread = max(ys) - min(ys)
        print(f"  {mod}: x={xs}, Δx={dxs}, y={ys}, y_spread={y_spread:.0f}d")

    print("\n[5] S1-TX1 origin")
    x0, y0 = lookup["S1-TX1"]
    print(f"  ({x0}d, {y0}d) → {'PASS' if x0 == 0 and y0 == 0 else 'FAIL'}")

    print("\n[6] VA span (192)")
    print(f"  x: {va_mm[:, 0].min():.2f} .. {va_mm[:, 0].max():.2f} mm")
    print(f"  y: {va_mm[:, 1].min():.2f} .. {va_mm[:, 1].max():.2f} mm")

    # --- plot: one panel per submodule ---
    fig, axes = plt.subplots(2, 2, figsize=(12, 11))
    fig.suptitle(
        f"Physical antenna per submodule  (d = λ/2 = {D_MM:.4f} mm, S1-TX1 = origin)",
        fontsize=13,
    )

    all_x: list[float] = []
    all_y: list[float] = []

    for ax, mod in zip(axes.flat, MODULE_ORDER):
        ants = modules[mod]
        txs = [a for a in ants if a.role == "TX"]
        rxs = [a for a in ants if a.role == "RX"]
        c = COLORS[mod]

        for a in txs:
            ax.scatter(a.x_d, a.y_d, c=c, marker="^", s=120, edgecolors="k", linewidths=0.7, zorder=3)
            ax.annotate(f"T{a.index}", (a.x_d, a.y_d), fontsize=9, ha="center", va="bottom", xytext=(0, 5), textcoords="offset points")
            all_x.append(a.x_d)
            all_y.append(a.y_d)

        for a in rxs:
            ax.scatter(a.x_d, a.y_d, c=c, marker="o", s=90, edgecolors="k", linewidths=0.7, zorder=3)
            ax.annotate(f"R{a.index}", (a.x_d, a.y_d), fontsize=9, ha="center", va="bottom", xytext=(0, 5), textcoords="offset points")
            all_x.append(a.x_d)
            all_y.append(a.y_d)

        if mod == "S1":
            ax.scatter(0, 0, marker="*", s=250, c="gold", edgecolors="k", zorder=4)

        # connect TX chain
        tx_xy = np.array([(a.x_d, a.y_d) for a in txs])
        ax.plot(tx_xy[:, 0], tx_xy[:, 1], "--", color=c, alpha=0.5, lw=1)

        ax.set_title(mod, fontsize=14, fontweight="bold", color=c)
        ax.set_xlabel("x (d = λ/2)")
        ax.set_ylabel("y (d = λ/2)")
        ax.set_aspect("equal")
        ax.grid(True, alpha=0.35)
        ax.axhline(0, color="gray", lw=0.6)
        ax.axvline(0, color="gray", lw=0.6)

    pad = 2
    for ax in axes.flat:
        ax.set_xlim(min(all_x) - pad, max(all_x) + pad)
        ax.set_ylim(min(all_y) - pad, max(all_y) + pad)

    out_sub = DOCS / "physical-antenna-plot.png"
    fig.tight_layout()
    fig.savefig(out_sub, dpi=150)

    # overview + VA
    fig2, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 7))
    fig2.suptitle("Overview (all submodules) + 192 virtual apertures", fontsize=13)

    for mod in MODULE_ORDER:
        for a in modules[mod]:
            mk = "^" if a.role == "TX" else "o"
            ax1.scatter(a.x_d, a.y_d, c=COLORS[mod], marker=mk, s=80, edgecolors="k", linewidths=0.5)
        # submodule label at TX1
        tx1 = lookup[f"{mod}-TX1"]
        ax1.annotate(mod, tx1, fontsize=11, fontweight="bold", color=COLORS[mod], xytext=(6, 6), textcoords="offset points")

    ax1.scatter(0, 0, marker="*", s=200, c="gold", edgecolors="k", zorder=5)
    ax1.set_xlabel("x (d = λ/2)")
    ax1.set_ylabel("y (d = λ/2)")
    ax1.set_title("TX ▲  RX ●  (★ = S1-TX1 origin)")
    ax1.set_aspect("equal")
    ax1.grid(True, alpha=0.3)

    sc = ax2.scatter(va_mm[:, 0], va_mm[:, 1], s=16, c=va_mm[:, 1], cmap="viridis", alpha=0.9)
    fig2.colorbar(sc, ax=ax2, label="y (mm)")
    ax2.set_xlabel("x (mm)")
    ax2.set_ylabel("y (mm)")
    ax2.set_title("192 VA (TX+RX, cube tile×16+sub)")
    ax2.set_aspect("equal")
    ax2.grid(True, alpha=0.3)

    handles = [mpatches.Patch(color=COLORS[m], label=m) for m in MODULE_ORDER]
    ax1.legend(handles=handles, loc="upper left")

    out_overview = DOCS / "physical-antenna-overview.png"
    fig2.tight_layout()
    fig2.savefig(out_overview, dpi=150)

    print(f"\nSaved {out_sub}")
    print(f"Saved {out_overview}")


if __name__ == "__main__":
    main()
