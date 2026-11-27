#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn a bench/run.sh results directory into a Markdown table and an SVG chart.

Usage: bench/plot.py bench/results/<build id> [--svg FILE]

Reads the raw per-call latencies (<label>.ns, nanoseconds) and summary.txt, prints a
Markdown table on stdout and writes latency.svg (or --svg FILE) into the directory: one
row per run on a logarithmic time axis, a bar from the minimum to the median, a thin line
on to the maximum and a tick at p99. Standard library only.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

ORDER = ["qnx-quiet", "qnx-fixed-quiet", "uds-quiet", "qnx-hog", "qnx-fixed-hog", "uds-hog"]
TITLES = {
    "qnx-quiet": "native, idle CPU",
    "qnx-fixed-quiet": "native without inheritance, idle CPU",
    "uds-quiet": "UDS, idle CPU",
    "qnx-hog": "native, priority-15 hog",
    "qnx-fixed-hog": "native without inheritance, hog",
    "uds-hog": "UDS, hog",
}


def percentile(sorted_ns: list[int], permille: int) -> int:
    """Nearest rank, the same rule as the benchmark itself."""
    idx = (len(sorted_ns) * permille + 999) // 1000
    return sorted_ns[max(idx, 1) - 1]


def fmt_us(ns: float) -> str:
    us = ns / 1000.0
    if us >= 1000.0:
        return f"{us / 1000.0:.1f} ms"
    return f"{us:.1f} µs"


def load(directory: Path) -> dict[str, list[int]]:
    runs = {}
    for path in directory.glob("*.ns"):
        values = sorted(int(line) for line in path.read_text().split())
        if values:
            runs[path.stem] = values
    return runs


def table(runs: dict[str, list[int]], labels: list[str]) -> str:
    lines = [
        "| Run | Calls | Min | Median | p99 | Max | Calls over 1 ms |",
        "|-----|-------|-----|--------|-----|-----|-----------------|",
    ]
    for label in labels:
        v = runs[label]
        over = sum(1 for x in v if x > 1_000_000)
        lines.append(
            f"| {TITLES.get(label, label)} (`{label}`) | {len(v)} | {fmt_us(v[0])} | "
            f"{fmt_us(percentile(v, 500))} | {fmt_us(percentile(v, 990))} | {fmt_us(v[-1])} | "
            f"{over} |"
        )
    return "\n".join(lines)


def svg(runs: dict[str, list[int]], labels: list[str], subtitle: str) -> str:
    left, right, top, row = 300, 110, 70, 44
    width = 980
    height = top + row * len(labels) + 60
    lo_ns, hi_ns = 1_000.0, 100_000_000.0  # 1 µs .. 100 ms
    span = math.log10(hi_ns) - math.log10(lo_ns)

    def x(ns: float) -> float:
        clamped = min(max(ns, lo_ns), hi_ns)
        return left + (math.log10(clamped) - math.log10(lo_ns)) / span * (width - left - right)

    out = [
        (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'font-family="sans-serif" font-size="13">'
        ),
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        (
            '<text x="20" y="28" font-size="17" font-weight="bold" fill="#1a1a1a">'
            "Round-trip latency of a priority-30 caller, server at priority 10</text>"
        ),
        f'<text x="20" y="48" fill="#555555">{subtitle}</text>',
    ]
    bottom = top + row * len(labels)
    for exp, text in (
        (3, "1 µs"),
        (4, "10 µs"),
        (5, "100 µs"),
        (6, "1 ms"),
        (7, "10 ms"),
        (8, "100 ms"),
    ):
        gx = x(10.0**exp)
        out.append(
            f'<line x1="{gx:.1f}" y1="{top - 6}" x2="{gx:.1f}" y2="{bottom}" stroke="#dddddd"/>'
        )
        out.append(
            f'<text x="{gx:.1f}" y="{bottom + 20}" text-anchor="middle" fill="#555555">{text}</text>'
        )
    for i, label in enumerate(labels):
        v = runs[label]
        cy = top + row * i + row / 2
        colour = "#1f77b4" if label.startswith("qnx") and "fixed" not in label else "#c0392b"
        if label.endswith("quiet"):
            colour = "#7f8c8d" if colour == "#c0392b" else "#5dade2"
        p50, p99 = percentile(v, 500), percentile(v, 990)
        out.append(
            f'<text x="{left - 12}" y="{cy + 4:.1f}" text-anchor="end" fill="#1a1a1a">'
            f"{TITLES.get(label, label)}</text>"
        )
        out.append(
            f'<line x1="{x(v[0]):.1f}" y1="{cy:.1f}" x2="{x(v[-1]):.1f}" y2="{cy:.1f}" '
            f'stroke="{colour}" stroke-width="2"/>'
        )
        out.append(
            f'<rect x="{x(v[0]):.1f}" y="{cy - 9:.1f}" width="{max(x(p50) - x(v[0]), 3):.1f}" '
            f'height="18" fill="{colour}"/>'
        )
        out.append(
            f'<line x1="{x(p99):.1f}" y1="{cy - 12:.1f}" x2="{x(p99):.1f}" y2="{cy + 12:.1f}" '
            f'stroke="#1a1a1a" stroke-width="2"/>'
        )
        out.append(
            f'<text x="{x(v[-1]) + 8:.1f}" y="{cy + 4:.1f}" fill="#1a1a1a">'
            f"median {fmt_us(p50)}</text>"
        )
    out.append(
        f'<text x="20" y="{height - 12}" fill="#555555">bar: minimum to median · line: to maximum · '
        "tick: p99 · logarithmic axis</text>"
    )
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--svg", type=Path, default=None)
    args = parser.parse_args()
    runs = load(args.directory)
    if not runs:
        print(f"plot: no *.ns files in {args.directory}", file=sys.stderr)
        return 1
    labels = [name for name in ORDER if name in runs] + sorted(set(runs) - set(ORDER))
    subtitle = ""
    summary = args.directory / "summary.txt"
    if summary.exists():
        first = summary.read_text().splitlines()[0].split()
        fields = dict(f.split("=", 1) for f in first if "=" in f)
        subtitle = (
            f"qmp {fields.get('version', '?')} · {fields.get('calls', '?')} calls per run · hog "
            f"spins {_hog(summary, 'hog_burst_ms')} ms, sleeps {_hog(summary, 'hog_idle_ms')} ms"
        )
    print(table(runs, labels))
    target = args.svg if args.svg is not None else args.directory / "latency.svg"
    target.write_text(svg(runs, labels, subtitle))
    print(f"\nchart: {target}", file=sys.stderr)
    return 0


def _hog(summary: Path, key: str) -> str:
    """The hog setting of the first run that had one (quiet runs report a burst of 0)."""
    for line in summary.read_text().splitlines():
        fields = dict(f.split("=", 1) for f in line.split() if "=" in f)
        if fields.get("hog_burst_ms", "0") != "0":
            return fields.get(key, "?")
    return "?"


if __name__ == "__main__":
    sys.exit(main())
