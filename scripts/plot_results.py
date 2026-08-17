#!/usr/bin/env python3
"""Generate the three Phase 7 result plots as static PNGs.

Deliberately small: three plots, each carrying a distinct claim not already
stated as a README table, no more. Requires matplotlib (not a stdlib dep):

    python3 -m pip install matplotlib   # or: python3 -m venv .venv && \
                                         #     .venv/bin/pip install matplotlib

Usage:
    python3 scripts/plot_results.py
        [--benchmark-csv results/benchmark_summary.csv]
        [--accuracy-csv  results/accuracy_stages.csv]
        [--outdir results/plots]
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")  # headless -- no display needed in CI or over ssh
    import matplotlib.pyplot as plt
except ModuleNotFoundError:
    sys.exit(
        "matplotlib is required for plotting. Install it with:\n"
        "  python3 -m pip install matplotlib\n"
        "or, if pip refuses a system-wide install (PEP 668):\n"
        "  python3 -m venv .venv && .venv/bin/pip install matplotlib && "
        ".venv/bin/python3 scripts/plot_results.py"
    )

# Categorical slots 1/2 from the dataviz skill's validated reference palette
# (references/palette.md) -- blue for the primary series, orange reserved
# for anything that must read as "a different condition," never cycled.
BLUE = "#2a78d6"
ORANGE = "#eb6834"
GRAY = "#52514e"


def read_csv_rows(path: Path) -> list[dict]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def plot_accuracy_stages(accuracy_csv: Path, outdir: Path) -> None:
    rows = read_csv_rows(accuracy_csv)
    if not rows:
        print(f"skip: {accuracy_csv} has no rows", file=sys.stderr)
        return

    labels = [f"{r['stage']}\n({r['config']})" for r in rows]
    values = [float(r["accuracy_pct"]) for r in rows]
    # The default-spacing config is the primary series; any other config
    # (currently only the wide-spacing weighted+causal figure) is visually
    # distinguished -- it must never be silently read as the same measurement.
    colors = [BLUE if r["config"] == "default spacing" else ORANGE for r in rows]

    fig, ax = plt.subplots(figsize=(9, 5))
    y_pos = range(len(rows))
    ax.barh(y_pos, values, color=colors)
    ax.set_yticks(list(y_pos))
    ax.set_yticklabels(labels, fontsize=8)
    ax.set_xlabel("Pairwise ordering accuracy (%)")
    ax.set_xlim(0, 100)  # anchored at 0 -- a truncated axis on an accuracy
                          # chart is misleading, per the dataviz anti-patterns.
    ax.set_title("Ordering accuracy by pipeline stage")
    ax.invert_yaxis()
    for y, v in zip(y_pos, values):
        ax.text(v + 1, y, f"{v:.2f}%", va="center", fontsize=8, color=GRAY)
    fig.text(
        0.5, -0.02,
        "Orange bar is a DIFFERENT simulation config (--inter-event-us 500), not directly "
        "comparable to the default-spacing blue bars.",
        ha="center", fontsize=7, color=GRAY,
    )

    outpath = outdir / "accuracy_stages.png"
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {outpath}", file=sys.stderr)


def plot_reconstruction_throughput(benchmark_csv: Path, outdir: Path) -> None:
    rows = read_csv_rows(benchmark_csv)
    series = {
        "BM_Reconstruct_Pipeline": ([], []),
        "BM_EndToEnd_Full": ({}, {}),  # events -> list of items/sec, averaged across source counts
    }
    for r in rows:
        if r["family"] == "BM_Reconstruct_Pipeline" and r.get("items_per_second"):
            n = int(r["args"])
            series["BM_Reconstruct_Pipeline"][0].append(n)
            series["BM_Reconstruct_Pipeline"][1].append(float(r["items_per_second"]))
        elif r["family"] == "BM_EndToEnd_Full" and r.get("items_per_second"):
            # args is "sources/events"
            _, events = r["args"].split("/")
            n = int(events)
            series["BM_EndToEnd_Full"][0].setdefault(n, []).append(float(r["items_per_second"]))

    fig, ax = plt.subplots(figsize=(8, 5))

    x1, y1 = series["BM_Reconstruct_Pipeline"]
    if x1:
        pairs = sorted(zip(x1, y1))
        xs, ys = zip(*pairs)
        ax.plot(xs, ys, "o-", color=BLUE, label="Reconstruction only")
        ax.annotate("Reconstruction only", (xs[-1], ys[-1]), textcoords="offset points",
                    xytext=(6, 6), fontsize=8, color=BLUE)

    events_map = series["BM_EndToEnd_Full"][0]
    if events_map:
        xs2 = sorted(events_map)
        ys2 = [sum(events_map[n]) / len(events_map[n]) for n in xs2]  # mean across source counts
        ax.plot(xs2, ys2, "s--", color=ORANGE, label="End-to-end (sim + reconstruction)")
        ax.annotate("End-to-end", (xs2[-1], ys2[-1]), textcoords="offset points",
                    xytext=(6, -12), fontsize=8, color=ORANGE)

    ax.set_xscale("log")
    ax.set_xlabel("Events (log scale)")
    ax.set_ylabel("Items / second")
    ax.set_title("Reconstruction throughput vs. event count")
    ax.grid(True, which="major", axis="y", alpha=0.3)

    outpath = outdir / "reconstruction_throughput.png"
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {outpath}", file=sys.stderr)


def plot_async_delivery_scaling(benchmark_csv: Path, outdir: Path) -> None:
    rows = read_csv_rows(benchmark_csv)
    points = []
    for r in rows:
        if r["family"] == "BM_EndToEnd_AsyncDelivery":
            sources, _events = r["args"].split("/")
            points.append((int(sources), float(r["real_time_us"])))
    if not points:
        print("skip: no BM_EndToEnd_AsyncDelivery rows found", file=sys.stderr)
        return
    points.sort()
    xs, ys = zip(*points)

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.plot(xs, ys, "o-", color=BLUE)
    for x, y in points:
        ax.annotate(f"{y:.0f}us", (x, y), textcoords="offset points", xytext=(0, 8), fontsize=8, color=GRAY, ha="center")
    ax.set_xlabel("Source count")
    ax.set_ylabel("Delivery time (us)")
    ax.set_title("Async delivery time vs. source count (20,000 events)")
    ax.set_xticks(list(xs))
    ax.grid(True, axis="y", alpha=0.3)

    outpath = outdir / "async_delivery_scaling.png"
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {outpath}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--benchmark-csv", type=Path, default=Path("results/benchmark_summary.csv"))
    parser.add_argument("--accuracy-csv", type=Path, default=Path("results/accuracy_stages.csv"))
    parser.add_argument("--outdir", type=Path, default=Path("results/plots"))
    args = parser.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)

    if args.accuracy_csv.exists():
        plot_accuracy_stages(args.accuracy_csv, args.outdir)
    else:
        print(f"skip: {args.accuracy_csv} not found", file=sys.stderr)

    if args.benchmark_csv.exists():
        plot_reconstruction_throughput(args.benchmark_csv, args.outdir)
        plot_async_delivery_scaling(args.benchmark_csv, args.outdir)
    else:
        print(f"skip: {args.benchmark_csv} not found", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
