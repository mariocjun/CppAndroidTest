#!/usr/bin/env python3
"""Compare two cppbench JSON outputs side-by-side.

Usage:
    scripts/compare.py results/exynos-20260527.json results/snapdragon-20260527.json

Prints a table with per-cluster STREAM bandwidth and a ratio column. Designed
for quick eyeballing of Note10+ Exynos vs Snapdragon runs; no external deps.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def load(p: str) -> dict:
    return json.loads(Path(p).read_text())


def fmt_freq(khz: int) -> str:
    return f"{khz / 1e6:.2f} GHz"


def stream_rows(doc: dict) -> list[dict]:
    benches = doc.get("benchmarks", [])
    for b in benches:
        if b.get("name") == "stream":
            return b["result"]["per_cluster"]
    return []


def label(doc: dict) -> str:
    env = doc.get("env", {})
    return env.get("soc_identified") or env.get("ro_product_model") or "?"


def compare(a: dict, b: dict) -> None:
    la, lb = label(a), label(b)
    print(f"\n  Device A: {la}    Device B: {lb}\n")

    rows_a = stream_rows(a)
    rows_b = stream_rows(b)

    # Best-effort cluster pairing: assume same order (sorted by freq ascending).
    n = max(len(rows_a), len(rows_b))
    print(f"{'cluster':<12}{'metric':<12}{'A (GB/s)':>14}{'B (GB/s)':>14}{'B/A':>10}")
    print("-" * 62)
    for i in range(n):
        ra = rows_a[i] if i < len(rows_a) else None
        rb = rows_b[i] if i < len(rows_b) else None
        sa = ra["stream"] if ra else {}
        sb = rb["stream"] if rb else {}
        freq_a = fmt_freq(ra["max_freq_khz"]) if ra else "-"
        freq_b = fmt_freq(rb["max_freq_khz"]) if rb else "-"
        label = f"#{i} {freq_a if ra else freq_b}"
        for k in ("copy_gbps", "scale_gbps", "add_gbps", "triad_gbps"):
            va = sa.get(k)
            vb = sb.get(k)
            ratio = (vb / va) if (va and vb and va > 0) else None
            print(f"{label:<12}{k.replace('_gbps',''):<12}"
                  f"{(f'{va:.2f}' if va else '-'):>14}"
                  f"{(f'{vb:.2f}' if vb else '-'):>14}"
                  f"{(f'{ratio:.2f}x' if ratio else '-'):>10}")
            label = ""  # only print cluster label on first row
        print()


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    compare(load(sys.argv[1]), load(sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
