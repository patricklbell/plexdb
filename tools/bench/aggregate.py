#!/usr/bin/env python3
"""aggregate.py — combine per-rep parsed metrics into one result document.

Usage:
    aggregate.py <system_info.json> <reps_dir> <mode> > result.json

<reps_dir> contains "<target>.<workload>.jsonl" files, one JSON metrics
object per line (one line per repetition, in run order). The first
repetition is discarded as warm-up when more than one is present, and the
rest are combined with the per-metric median.
"""

import glob
import json
import os
import statistics
import sys
from datetime import datetime, timezone


def aggregate_reps(rows: list, discard_first: bool = True) -> dict:
    if discard_first and len(rows) > 1:
        rows = rows[1:]
    keys = rows[0].keys()
    return {k: statistics.median(r[k] for r in rows) for k in keys}


def build_result(host: dict, reps_dir: str, mode: str) -> dict:
    targets: dict = {}
    for path in sorted(glob.glob(os.path.join(reps_dir, "*.jsonl"))):
        name = os.path.basename(path)[: -len(".jsonl")]
        target, workload = name.split(".", 1)
        with open(path) as f:
            rows = [json.loads(line) for line in f if line.strip()]
        if not rows:
            continue
        targets.setdefault(target, {})[workload] = aggregate_reps(rows)
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "mode": mode,
        "host": host,
        "targets": targets,
    }


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <system_info.json> <reps_dir> <mode>", file=sys.stderr)
        return 1
    system_info_path, reps_dir, mode = sys.argv[1:4]
    with open(system_info_path) as f:
        host = json.load(f)
    result = build_result(host, reps_dir, mode)
    json.dump(result, sys.stdout, indent=2)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
