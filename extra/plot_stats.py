#!/usr/bin/env python3
"""Plot stats from the plexdb log_stat plugin output.

Usage:
    python extra/plot_stats.py [plexdb.stats]           # static plot
    python extra/plot_stats.py --live [plexdb.stats]    # real-time terminal view

The input file is produced by the objstore_log_stat plugin. Set the
environment variable PLEXDB_STAT_FILE to control the output path
(default: plexdb.stats), then load the plugin via LD_PRELOAD:

    PLEXDB_STAT_FILE=my.stats LD_PRELOAD=./build/objstore/libobjstore_log_stat.so \\
        ./build/objstore/objstore_server ...
    python extra/plot_stats.py my.stats

For the real-time terminal dashboard, prefer the native C++ dashboard plugin:
    LD_PRELOAD=./build/objstore/libobjstore_log_dashboard.so \\
        ./build/objstore/objstore_server ...

File format (tab-separated, one event per line):
    P <producer_id>\\t<name>                              — producer registration
    D <producer_id>\\t<key>\\t<value>                      — producer metadata
    M <producer_id>\\t<stat_id>\\t<name>                   — stat metadata
    S <producer_id>\\t<stat_id>\\t<timestamp_ns>\\t<value>  — stat sample
"""

import sys
import os
import time
import collections
from pathlib import Path


def parse_stats(path):
    """Parse a plexdb .stats file into structured data."""
    producers = {}      # producer_id -> name
    producer_meta = {}  # (producer_id, key) -> value
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

            elif tag == "D" and len(parts) >= 3:
                pid = int(parts[0])
                producer_meta[(pid, parts[1])] = parts[2]

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

    return producers, producer_meta, stat_meta, series


def label_for(producers, stat_meta, key):
    """Build a human-readable label for a series key."""
    pid, sid = key
    producer = producers.get(pid, f"producer:{pid}")
    stat = stat_meta.get(key, f"stat:{sid}")
    return f"{producer} / {stat}"


def render_live(path):
    """Tail the stats file and render a real-time terminal dashboard."""
    last_pos = 0
    producers = {}
    producer_meta = {}
    stat_meta = {}
    stat_values = {}

    while True:
        try:
            with open(path) as f:
                f.seek(last_pos)
                new_lines = f.readlines()
                last_pos = f.tell()
        except FileNotFoundError:
            time.sleep(0.5)
            continue

        for raw in new_lines:
            line = raw.rstrip("\n")
            if not line:
                continue
            tag = line[0]
            rest = line[2:]
            parts = rest.split("\t")

            if tag == "P" and len(parts) >= 2:
                producers[int(parts[0])] = parts[1]
            elif tag == "D" and len(parts) >= 3:
                producer_meta[(int(parts[0]), parts[1])] = parts[2]
            elif tag == "M" and len(parts) >= 3:
                stat_meta[(int(parts[0]), int(parts[1]))] = parts[2]
            elif tag == "S" and len(parts) >= 4:
                stat_values[(int(parts[0]), int(parts[1]))] = int(parts[3])

        # render
        sys.stdout.write("\033[H\033[2J")
        sys.stdout.write("\033[1;36m=== plexdb stats (live) ===\033[0m\n\n")

        by_producer = collections.defaultdict(list)
        for key in sorted(stat_values):
            by_producer[key[0]].append(key)

        for pid, keys in by_producer.items():
            pname = producers.get(pid, f"producer:{pid}")
            meta_str = " ".join(
                f"\033[2m{k}={v}\033[0m"
                for (mid, k), v in sorted(producer_meta.items())
                if mid == pid
            )
            sys.stdout.write(f"  \033[1;33m{pname}\033[0m (id={pid}) {meta_str}\n")
            for key in keys:
                sname = stat_meta.get(key, f"stat:{key[1]}")
                val = stat_values[key]
                sys.stdout.write(f"    {sname:<30s} \033[1;32m{val}\033[0m\n")
            sys.stdout.write("\n")

        if not stat_values:
            sys.stdout.write("  \033[2m(waiting for stats...)\033[0m\n")

        sys.stdout.flush()
        time.sleep(0.5)


def plot_static(path):
    """Static matplotlib plot."""
    producers, producer_meta, stat_meta, series = parse_stats(path)

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


def main():
    live = "--live" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--live"]
    path = args[0] if args else "plexdb.stats"

    if not live and not Path(path).exists():
        print(f"error: file not found: {path}", file=sys.stderr)
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    if live:
        render_live(path)
    else:
        plot_static(path)


if __name__ == "__main__":
    main()
