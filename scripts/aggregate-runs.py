#!/usr/bin/env python3
"""Aggregate N cppbench JSON runs into median + spread per cluster/metric.

A single cppbench run already does best-of-N *iterations* internally, but
mobile DVFS + scheduler interference make a single RUN noisy (observed: the
Exynos 9825 A75 mid-cluster reported 14.3 GFLOPS in one run and 19.1 in the
next — ~34% spread). This tool runs the statistics across multiple runs so we
report a defensible median with a coefficient of variation (CV) that flags
which numbers are stable vs. which are measurement noise.

Usage:
    python scripts/aggregate-runs.py results/N975F-Exynos9825-default-*.json

Reports, per (benchmark, cluster, metric):
    median, min, max, CV%  (CV = stdev/mean; >15% = noisy, treat with caution)
"""
from __future__ import annotations

import glob
import json
import statistics
import sys
from collections import defaultdict


# (benchmark name) -> list of (per_cluster sub-key, [scalar metric keys])
SCALAR_METRICS = {
    "stream":   ("stream",   ["copy_gbps", "scale_gbps", "add_gbps", "triad_gbps"]),
    "neon_fma": ("neon_fma", ["fp32_gflops_per_core", "fp16_gflops_per_core"]),
    "dot_int8": ("dot_int8", ["sdot_gops_per_core", "udot_gops_per_core"]),
    "i8mm":     ("i8mm",     ["smmla_gops_per_core"]),
    "sve2":     ("sve2",     ["fp32_gflops_per_core"]),
}


def cluster_key(pc: dict) -> str:
    idx = pc.get("cluster_idx", "?")
    f = pc.get("max_freq_khz", 0) / 1e6
    return f"c{idx}@{f:.2f}GHz"


def collect(paths: list[str]) -> dict:
    # samples[(bench, cluster, metric)] = [values across runs]
    samples: dict[tuple, list[float]] = defaultdict(list)
    n_runs = 0
    soc = None
    for p in paths:
        try:
            doc = json.loads(open(p, encoding="utf-8").read())
        except Exception as e:
            print(f"skip {p}: {e}", file=sys.stderr)
            continue
        n_runs += 1
        soc = doc.get("env", {}).get("soc_identified", soc)
        for b in doc.get("benchmarks", []):
            name = b.get("name")
            if name not in SCALAR_METRICS:
                continue
            subkey, metrics = SCALAR_METRICS[name]
            for pc in b.get("result", {}).get("per_cluster", []):
                ck = cluster_key(pc)
                sub = pc.get(subkey, {})
                for m in metrics:
                    v = sub.get(m)
                    # Skip the -1 sentinel (feature unsupported on this device).
                    if isinstance(v, (int, float)) and v >= 0:
                        samples[(name, ck, m)].append(float(v))
    return {"samples": samples, "n_runs": n_runs, "soc": soc}


def report(agg: dict) -> None:
    soc = agg["soc"]
    n = agg["n_runs"]
    print(f"\nSoC: {soc}   runs aggregated: {n}\n")
    if n == 0:
        print("no runs found")
        return
    header = f"{'benchmark':<10}{'cluster':<16}{'metric':<22}{'median':>10}{'min':>9}{'max':>9}{'CV%':>7}"
    print(header)
    print("-" * len(header))
    last_bench = None
    for (bench, cluster, metric), vals in sorted(agg["samples"].items()):
        if not vals:
            continue
        med = statistics.median(vals)
        lo, hi = min(vals), max(vals)
        cv = (statistics.pstdev(vals) / statistics.mean(vals) * 100) if statistics.mean(vals) else 0.0
        flag = "  <-- noisy" if cv > 15 else ""
        bench_col = bench if bench != last_bench else ""
        last_bench = bench
        print(f"{bench_col:<10}{cluster:<16}{metric:<22}{med:>10.2f}{lo:>9.2f}{hi:>9.2f}{cv:>6.1f}%{flag}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    paths: list[str] = []
    for arg in sys.argv[1:]:
        paths.extend(glob.glob(arg))
    if not paths:
        print("no files matched", file=sys.stderr)
        return 1
    report(collect(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())
