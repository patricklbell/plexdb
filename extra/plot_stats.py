#!/usr/bin/env python3
"""Plot stats from the plexdb log_stat plugin output.

Usage:
    python extra/plot_stats.py [plexdb.stats]

The input file is produced by the objstore_log_stat plugin. Set the
environment variable PLEXDB_STAT_FILE to control the output path
(default: plexdb.stats), then load the plugin via LD_PRELOAD:

    PLEXDB_STAT_FILE=my.stats LD_PRELOAD=./build/objstore/libobjstore_log_stat.so \\
        ./build/objstore/objstore_server ...
    python extra/plot_stats.py my.stats

File format (tab-separated, one event per line):
    P <producer_id>\\t<name>                              — producer registration
    M <producer_id>\\t<stat_id>\\t<name>                   — stat metadata
    S <producer_id>\\t<stat_id>\\t<timestamp_ns>\\t<value>  — stat sample
"""

import sys
import collections
from pathlib import Path

def parse_stats(path):
    """Parse a plexdb .stats file into structured data."""
    producers = {}      # producer_id -> name
    stat_meta = {}      # (producer_id, stat_id) -> name
    # series key = (producer_id, stat_id)
    series = collections.defaultdict(lambda: {"ts": [], "values": []})

    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            tag = line[0]
            rest = line[2:]  # skip "X "
            parts = rest.split("\t")

            if tag == "P" and len(parts) >= 2:
                pid = int(parts[0])
                producers[pid] = parts[1]

            elif tag == "M" and len(parts) >= 3:
                pid = int(parts[0])
                sid = int(parts[1])
                stat_meta[(pid, sid)] = parts[2]

            elif tag == "S" and len(parts) >= 4:
                pid = int(parts[0])
                sid = int(parts[1])
                ts_ns = int(parts[2])
                value = int(parts[3])
                key = (pid, sid)
                series[key]["ts"].append(ts_ns)
                series[key]["values"].append(value)

    return producers, stat_meta, series


def label_for(producers, stat_meta, key):
    """Build a human-readable label for a series key."""
    pid, sid = key
    producer = producers.get(pid, f"producer:{pid}")
    stat = stat_meta.get(key, f"stat:{sid}")
    return f"{producer} / {stat}"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "plexdb.stats"
    if not Path(path).exists():
        print(f"error: file not found: {path}", file=sys.stderr)
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    producers, stat_meta, series = parse_stats(path)

    if not series:
        print("No stat samples found in", path)
        sys.exit(0)

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("error: matplotlib is required.  pip install matplotlib", file=sys.stderr)
        sys.exit(1)

    fig, ax = plt.subplots(figsize=(12, 6))

    for key, data in sorted(series.items()):
        ts = data["ts"]
        vals = data["values"]
        # convert nanoseconds to seconds relative to first sample
        t0 = ts[0]
        t_sec = [(t - t0) / 1e9 for t in ts]
        ax.plot(t_sec, vals, label=label_for(producers, stat_meta, key), marker=".", markersize=3)

    ax.set_xlabel("Time (seconds)")
    ax.set_ylabel("Value")
    ax.set_title("plexdb stats")
    ax.legend(loc="best", fontsize="small")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
