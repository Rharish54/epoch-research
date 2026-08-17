#!/usr/bin/env python3
"""Reshape Google Benchmark JSON output into markdown tables and a flat CSV.

Scope, deliberately: this script reads ONLY the new benchmark-suite JSON
produced by estimator_benchmark/reconstruction_benchmark (via
--benchmark_format=json). It does NOT parse the existing results/phase*.txt
prose reports -- those numbers are already final, already transcribed into
the README, and each is traceable to a committed raw file. Writing a prose
parser for them would add a fragile new component whose only output is text
that already exists, for zero new information.

Usage:
    python3 scripts/analyze_results.py results/benchmark_*.json \
        [--csv results/benchmark_summary.csv] [--format markdown|text]

Stdlib only -- no pandas/numpy. The whole job is reshaping a JSON array into
a table.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

# Fields already surfaced as named columns; anything else in a benchmark
# entry (e.g. BM_PerEventCorrectionLatency's p50_ns/p95_ns/p99_ns counters)
# is picked up automatically as an "extra" column, with no per-counter code.
KNOWN_FIELDS = {
    "name", "family_index", "per_family_instance_index", "run_name",
    "run_type", "repetitions", "threads", "aggregate_name", "aggregate_unit",
    "iterations", "real_time", "cpu_time", "time_unit", "items_per_second",
}

TIME_UNIT_TO_US = {"ns": 1e-3, "us": 1.0, "ms": 1e3, "s": 1e6}


def load_benchmark_json(path: Path) -> dict:
    with path.open() as f:
        return json.load(f)


def print_environment_block(data: dict, source: Path) -> None:
    ctx = data.get("context", {})
    print(f"### Environment ({source.name})\n")
    print(f"- Host: {ctx.get('host_name', '?')}")
    print(f"- CPUs: {ctx.get('num_cpus', '?')} @ {ctx.get('mhz_per_cpu', '?')} MHz")
    print(f"- Date: {ctx.get('date', '?')}")
    print(f"- Google Benchmark: {ctx.get('library_version', '?')} ({ctx.get('library_build_type', '?')})")
    print()

    build_type = ctx.get("library_build_type", "")
    if build_type != "release":
        print(
            f"WARNING: library_build_type is '{build_type}', not 'release'. "
            "Per CLAUDE.md's Benchmarking Rules, benchmarks must be Release "
            "builds -- these numbers should NOT be used.",
            file=sys.stderr,
        )
    if ctx.get("cpu_scaling_enabled"):
        print(
            "WARNING: cpu_scaling_enabled is true -- frequency scaling can "
            "add noise to timing measurements.",
            file=sys.stderr,
        )


def family_of(run_name: str) -> str:
    """BM_CausalGraph_Build/5000 -> BM_CausalGraph_Build"""
    return run_name.split("/", 1)[0]


def args_of(run_name: str) -> str:
    """BM_CausalGraph_Build/5000 -> 5000"""
    parts = run_name.split("/", 1)
    return parts[1] if len(parts) > 1 else ""


def normalize_time_us(value: float, unit: str) -> float:
    return value * TIME_UNIT_TO_US.get(unit, 1.0)


def collect_rows(data: dict) -> list[dict]:
    """One row per benchmark, preferring the 'mean' aggregate when repetitions
    were used (skipping 'median'/'stddev'/'cv' aggregate rows -- those exist
    for statistical context, not as additional data points), else the raw
    'iteration' row."""
    rows = []
    for b in data.get("benchmarks", []):
        run_type = b.get("run_type", "iteration")
        if run_type == "aggregate" and b.get("aggregate_name") != "mean":
            continue

        run_name = b.get("run_name", b.get("name", ""))
        row = {
            "family": family_of(run_name),
            "args": args_of(run_name),
            "real_time_us": normalize_time_us(b.get("real_time", 0.0), b.get("time_unit", "us")),
            "cpu_time_us": normalize_time_us(b.get("cpu_time", 0.0), b.get("time_unit", "us")),
            "iterations": b.get("iterations", 0),
            "items_per_second": b.get("items_per_second"),
        }
        for k, v in b.items():
            if k not in KNOWN_FIELDS:
                row[k] = v
        rows.append(row)
    return rows


def format_number(value) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        if abs(value) >= 1000:
            return f"{value:,.0f}"
        return f"{value:.3f}"
    return str(value)


def print_markdown_tables(rows: list[dict]) -> None:
    families: dict[str, list[dict]] = {}
    for r in rows:
        families.setdefault(r["family"], []).append(r)

    extra_cols: set[str] = set()
    for r in rows:
        extra_cols.update(k for k in r if k not in ("family", "args", "real_time_us", "cpu_time_us", "iterations", "items_per_second"))
    extra_cols = sorted(extra_cols)

    for family, family_rows in families.items():
        print(f"#### `{family}`\n")
        header = ["args", "real_time (us)", "cpu_time (us)", "iterations", "items/sec"] + extra_cols
        print("| " + " | ".join(header) + " |")
        print("|" + "---|" * len(header))
        for r in family_rows:
            cells = [
                r["args"],
                format_number(r["real_time_us"]),
                format_number(r["cpu_time_us"]),
                format_number(r["iterations"]),
                format_number(r["items_per_second"]),
            ] + [format_number(r.get(c)) for c in extra_cols]
            print("| " + " | ".join(cells) + " |")
        print()


def write_csv(rows: list[dict], path: Path) -> None:
    extra_cols: set[str] = set()
    for r in rows:
        extra_cols.update(k for k in r if k not in ("family", "args", "real_time_us", "cpu_time_us", "iterations", "items_per_second"))
    extra_cols = sorted(extra_cols)

    fieldnames = ["family", "args", "real_time_us", "cpu_time_us", "iterations", "items_per_second"] + extra_cols
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in rows:
            writer.writerow({k: r.get(k, "") for k in fieldnames})
    print(f"Wrote {path} ({len(rows)} rows)", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("json_files", nargs="+", type=Path, help="Google Benchmark JSON output file(s)")
    parser.add_argument("--csv", type=Path, help="write a flat summary CSV here (for plot_results.py)")
    parser.add_argument("--format", choices=["markdown", "text"], default="markdown")
    args = parser.parse_args()

    all_rows: list[dict] = []
    for path in args.json_files:
        data = load_benchmark_json(path)
        print_environment_block(data, path)
        rows = collect_rows(data)
        all_rows.extend(rows)
        if args.format == "markdown":
            print_markdown_tables(rows)

    if args.csv:
        write_csv(all_rows, args.csv)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
