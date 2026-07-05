#!/usr/bin/env python3
"""parse_stress.py — turn raw cassandra-stress stdout into structured metrics.

Usage:
    cassandra-stress write n=... | python3 tools/bench/parse_stress.py

Reads stdout from stdin (or a file given as argv[1]) and prints one JSON
object to stdout: {ops_per_sec, mean_latency_ms, p50, p95, p99, p999, errors}.
"""

import json
import re
import sys

_FIELDS = {
    "ops_per_sec": r"^Op rate\s*:\s*([\d,]+) op/s",
    "mean_latency_ms": r"^Latency mean\s*:\s*([\d,.]+) ms",
    "p50": r"^Latency median\s*:\s*([\d,.]+) ms",
    "p95": r"^Latency 95th percentile\s*:\s*([\d,.]+) ms",
    "p99": r"^Latency 99th percentile\s*:\s*([\d,.]+) ms",
    "p999": r"^Latency 99\.9th percentile\s*:\s*([\d,.]+) ms",
    "errors": r"^Total errors\s*:\s*([\d,]+)",
}


def parse(text: str) -> dict:
    """Extract metrics from the last 'Results:' block in cassandra-stress output.

    Raises ValueError if no Results block is found or a required field is missing.
    """
    idx = text.rfind("Results:")
    if idx == -1:
        raise ValueError("no 'Results:' block found in cassandra-stress output")
    block = text[idx:]

    values = {}
    for name, pattern in _FIELDS.items():
        m = re.search(pattern, block, re.MULTILINE)
        if not m:
            raise ValueError(f"missing field '{name}' in Results block")
        raw = m.group(1).replace(",", "")
        values[name] = int(raw) if name in ("ops_per_sec", "errors") else float(raw)
    return values


def main() -> int:
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    try:
        metrics = parse(text)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    json.dump(metrics, sys.stdout)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
