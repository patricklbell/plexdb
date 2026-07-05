#!/usr/bin/env python3
"""compare.py — diff a benchmark result against a per-machine baseline.

Mirrors the mustpass regression pattern in tools/cql_tests/run.sh, but for
numeric performance metrics with a tolerance instead of exact pass/fail.

Usage:
    compare.py <result.json> <baseline.json> [--tolerance PCT]
    compare.py --self-check <result_a.json> <result_b.json> [--tolerance PCT]

Exit codes:
    0   no regressions (or no significant self-check divergence)
    1   regression(s) found
    2   result and baseline are for different hardware (refuse to compare)
"""

import argparse
import json
import re
import sys

HIGHER_IS_BETTER = {"ops_per_sec"}
LOWER_IS_BETTER = {"mean_latency_ms", "p50", "p95", "p99", "p999", "errors"}
ALL_METRICS = HIGHER_IS_BETTER | LOWER_IS_BETTER

DEFAULT_TOLERANCE = 10.0
SELF_CHECK_TOLERANCE = 5.0


def slug_of(system_info: dict) -> str:
    """Derive a stable per-hardware-class identifier from a system_info.sh document."""
    model = system_info["cpu"]["model"]
    threads = system_info["cpu"]["logical_cpus"]
    s = re.sub(r"\b(cpu|processor)\b", "", model.lower())
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    return f"{s}-{threads}t"


def pct_delta(current: float, baseline: float):
    """Percent change from baseline to current, or None if baseline is zero."""
    if baseline == 0:
        return None
    return (current - baseline) / baseline * 100.0


def is_regression(metric: str, current: float, baseline: float, tolerance: float) -> bool:
    delta = pct_delta(current, baseline)
    if delta is None:
        # baseline is zero (most commonly `errors`): any nonzero current is notable.
        return current > baseline
    if metric in HIGHER_IS_BETTER:
        return delta < -tolerance
    return delta > tolerance


def compare_workload(current: dict, baseline: dict, tolerance: float, label: str):
    """Compare one {target}/{workload} metrics dict pair. Returns (rows, regressions)."""
    rows = []
    regressions = []
    for metric in ALL_METRICS:
        if metric not in current or metric not in baseline:
            continue
        cur, base = current[metric], baseline[metric]
        delta = pct_delta(cur, base)
        regressed = is_regression(metric, cur, base, tolerance)
        rows.append((label, metric, base, cur, delta, regressed))
        if regressed:
            regressions.append((label, metric, base, cur, delta))
    return rows, regressions


def compare_result(result: dict, baseline: dict, tolerance: float):
    """Compare a full result document against a full baseline document.

    Returns (rows, regressions, warnings).
    """
    warnings = []
    result_slug = slug_of(result["host"])
    if result_slug != baseline.get("slug"):
        raise SystemExit(
            f"error: result is for '{result_slug}' but baseline is for "
            f"'{baseline.get('slug')}' — refusing to compare across different hardware"
        )
    if result["host"]["hostname"] != baseline.get("hostname"):
        warnings.append(
            f"warning: comparing against a baseline captured on a different host "
            f"({baseline.get('hostname')}) of the same hardware class"
        )

    all_rows = []
    all_regressions = []
    for target, workloads in result.get("targets", {}).items():
        base_target = baseline.get("targets", {}).get(target)
        if not base_target:
            continue
        for workload, metrics in workloads.items():
            base_metrics = base_target.get(workload)
            if not base_metrics:
                continue
            rows, regressions = compare_workload(
                metrics, base_metrics, tolerance, f"{target}/{workload}"
            )
            all_rows.extend(rows)
            all_regressions.extend(regressions)
    return all_rows, all_regressions, warnings


def compare_self_check(result_a: dict, result_b: dict, tolerance: float):
    """Compare the plexdb target between two full result documents (e.g.
    container-mode vs native-mode), regardless of which direction is 'worse'.
    """
    a = result_a["targets"]["plexdb"]
    b = result_b["targets"]["plexdb"]
    rows = []
    divergences = []
    for workload, a_metrics in a.items():
        b_metrics = b.get(workload)
        if not b_metrics:
            continue
        for metric in ALL_METRICS:
            if metric not in a_metrics or metric not in b_metrics:
                continue
            va, vb = a_metrics[metric], b_metrics[metric]
            delta = pct_delta(vb, va)
            diverged = delta is not None and abs(delta) > tolerance
            rows.append((f"plexdb/{workload}", metric, va, vb, delta, diverged))
            if diverged:
                divergences.append((f"plexdb/{workload}", metric, va, vb, delta))
    return rows, divergences


def print_table(rows, baseline_label="baseline", current_label="current"):
    header = f"{'metric':<22} {baseline_label:>12} {current_label:>12} {'delta':>10}  "
    print(header)
    print("-" * len(header))
    for label, metric, base, cur, delta, flagged in rows:
        delta_str = f"{delta:+.1f}%" if delta is not None else "n/a"
        marker = " !!" if flagged else ""
        print(f"{label}/{metric:<12} {base:>12.2f} {cur:>12.2f} {delta_str:>10}{marker}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file_a")
    ap.add_argument("file_b")
    ap.add_argument("--tolerance", type=float, default=None)
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()

    with open(args.file_a) as f:
        doc_a = json.load(f)
    with open(args.file_b) as f:
        doc_b = json.load(f)

    if args.self_check:
        tolerance = args.tolerance if args.tolerance is not None else SELF_CHECK_TOLERANCE
        rows, divergences = compare_self_check(doc_a, doc_b, tolerance)
        print_table(rows, baseline_label="a", current_label="b")
        if divergences:
            print(f"\nSelf-check: {len(divergences)} metric(s) diverge by more than "
                  f"{tolerance}% between the two runs.")
            return 1
        print(f"\nSelf-check OK (within {tolerance}%).")
        return 0

    tolerance = args.tolerance if args.tolerance is not None else DEFAULT_TOLERANCE
    rows, regressions, warnings = compare_result(doc_a, doc_b, tolerance)
    for w in warnings:
        print(w, file=sys.stderr)
    print_table(rows)
    if regressions:
        print(f"\nREGRESSIONS — {len(regressions)} metric(s) beyond {tolerance}% tolerance:")
        for label, metric, base, cur, delta in regressions:
            print(f"  {label}/{metric}: {base:.2f} -> {cur:.2f} ({delta:+.1f}%)"
                  if delta is not None else f"  {label}/{metric}: {base:.2f} -> {cur:.2f}")
        return 1
    print("\nNo regressions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
