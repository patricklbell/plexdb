#!/usr/bin/env bash
# tune.sh — reversible, session-scoped Linux tuning for reproducible benchmarks.
#
# Only changes things that can be undone without a reboot (cpu governor,
# boost, swappiness, and marking the bench scratch dir nodatacow on btrfs).
# More invasive changes (kernel cmdline cpu isolation, disabling SMT,
# locking a fixed clock, moving the bench dir off a compressing filesystem,
# disabling swap outright) are never applied automatically — `--check`
# prints exact instructions for those instead, so a human decides.
#
# Usage:
#   ./tools/bench/tune.sh --check | --apply | --reset [options]
#     -d <dir>       bench scratch/data dir              (default: /tmp)
#     -C <cpulist>   cpu list the bench will pin to, used only to fill in
#                    the recommended-invasive-changes commands            (default: none)
#     -h             show this help
#
# State written by --apply (for --reset to undo) lives in:
#   ~/.cache/plexdb-bench/tune_state.json

set -euo pipefail

BENCH_DIR="/tmp"
CPULIST=""
ACTION=""
STATE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/plexdb-bench"
STATE_FILE="$STATE_DIR/tune_state.json"

while [ $# -gt 0 ]; do
    case "$1" in
        --check|--apply|--reset) ACTION="${1#--}" ;;
        -d) BENCH_DIR="$2"; shift ;;
        -C) CPULIST="$2"; shift ;;
        -h)
            sed -n '2,17p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
    shift
done

if [ -z "$ACTION" ]; then
    echo "error: one of --check, --apply, --reset is required (-h for help)" >&2
    exit 1
fi

governor_path() { echo "/sys/devices/system/cpu/cpu$1/cpufreq/scaling_governor"; }
all_cpus() { seq 0 "$(($(nproc) - 1))"; }

current_governor() { cat "$(governor_path 0)" 2>/dev/null || echo unknown; }
current_boost() { cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo unknown; }
current_swappiness() { cat /proc/sys/vm/swappiness 2>/dev/null || echo unknown; }
current_smt() { cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo unknown; }

print_invasive_recommendations() {
    local cpus="${CPULIST:-<cpulist, e.g. 2-9>}"
    cat <<EOF

── Recommended invasive changes (not applied — do these by hand) ─────────

1. Reserve the pinned benchmark cores from the scheduler and timer ticks.
   Requires a reboot; on this Fedora host:
     sudo grubby --update-kernel=ALL --args="isolcpus=${cpus} nohz_full=${cpus} rcu_nocbs=${cpus}"
     sudo reboot
   Why: removes scheduler migrations and timer-tick jitter from the pinned
   cores entirely, instead of just hinting affinity with taskset/cpuset.

2. Disable SMT (hyperthread siblings) so pinned "cores" are real cores:
     echo off | sudo tee /sys/devices/system/cpu/smt/control
   Reversible without reboot (echo on to restore), but affects the whole
   machine while set — not done automatically. Skip this if the bench
   workload is intentionally testing SMT-enabled throughput.

3. If boost-induced clock variance makes runs noisy, lock a fixed
   non-turbo frequency instead of toggling boost on/off:
     sudo cpupower frequency-set -f <freq, e.g. 4.2GHz>
   Trade-off: caps peak performance in exchange for run-to-run repeatability.

4. If \`--check\` above flags the bench dir as compressing/COW and the
   \`chattr +C\` workaround didn't fully apply (e.g. the dir already had
   files), move it to a dedicated filesystem instead:
     sudo mkfs.ext4 /dev/<scratch-partition>
     sudo mount -o noatime /dev/<scratch-partition> ${BENCH_DIR}

5. Disable swap outright for the duration of a run, on machines with less
   headroom than plenty of free RAM:
     sudo swapoff -a   # before the run
     sudo swapon -a    # after
EOF
}

do_check() {
    echo "── Reversible tuning (this machine) ───────────────────────────────"
    printf '  %-16s %-14s recommended: performance\n' "governor" "$(current_governor)"
    printf '  %-16s %-14s recommended: 1 (on)\n' "boost" "$(current_boost)"
    printf '  %-16s %-14s recommended: low (<=10) during a run\n' "swappiness" "$(current_swappiness)"
    printf '  %-16s %-14s\n' "smt_control" "$(current_smt)"

    if [ -d "$BENCH_DIR" ]; then
        local fs_type
        fs_type="$(findmnt -no FSTYPE --target "$BENCH_DIR" 2>/dev/null || echo unknown)"
        echo "  bench dir '$BENCH_DIR' is on fstype: $fs_type"
        if [ "$fs_type" = "btrfs" ]; then
            if lsattr -d "$BENCH_DIR" 2>/dev/null | grep -q 'C'; then
                echo "    already nodatacow (compression/COW disabled for this dir)"
            else
                echo "    NOT nodatacow yet — 'tune.sh --apply' will attempt 'chattr +C' (only works on an empty dir)"
            fi
        fi
    fi

    print_invasive_recommendations
}

do_apply() {
    mkdir -p "$STATE_DIR"
    if [ -f "$STATE_FILE" ]; then
        echo "warning: $STATE_FILE already exists — refusing to overwrite a saved state." >&2
        echo "Run --reset first, or remove the file if it's stale." >&2
        exit 1
    fi

    local prev_governor prev_boost prev_swappiness
    prev_governor="$(current_governor)"
    prev_boost="$(current_boost)"
    prev_swappiness="$(current_swappiness)"

    echo "Applying (sudo required)..."
    for cpu in $(all_cpus); do
        echo performance | sudo tee "$(governor_path "$cpu")" >/dev/null 2>&1 || true
    done
    [ -e /sys/devices/system/cpu/cpufreq/boost ] && echo 1 | sudo tee /sys/devices/system/cpu/cpufreq/boost >/dev/null 2>&1 || true
    echo 10 | sudo tee /proc/sys/vm/swappiness >/dev/null

    if [ -d "$BENCH_DIR" ]; then
        local fs_type
        fs_type="$(findmnt -no FSTYPE --target "$BENCH_DIR" 2>/dev/null || echo unknown)"
        if [ "$fs_type" = "btrfs" ]; then
            chattr +C "$BENCH_DIR" 2>/dev/null \
                && echo "Marked $BENCH_DIR nodatacow." \
                || echo "warning: chattr +C failed on $BENCH_DIR (non-empty dir?) — see recommendation #4 with --check." >&2
        fi
    fi

    python3 -c '
import json, sys
json.dump({"governor": sys.argv[1], "boost": sys.argv[2], "swappiness": sys.argv[3]}, open(sys.argv[4], "w"))
' "$prev_governor" "$prev_boost" "$prev_swappiness" "$STATE_FILE"
    echo "Saved previous state to $STATE_FILE"
    echo "Done. Run 'tune.sh --reset' to restore."
}

do_reset() {
    if [ ! -f "$STATE_FILE" ]; then
        echo "error: no saved state at $STATE_FILE (did you run --apply?)" >&2
        exit 1
    fi

    local prev_governor prev_boost prev_swappiness
    prev_governor="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["governor"])' "$STATE_FILE")"
    prev_boost="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["boost"])' "$STATE_FILE")"
    prev_swappiness="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["swappiness"])' "$STATE_FILE")"

    echo "Restoring (sudo required)..."
    if [ "$prev_governor" != "unknown" ]; then
        for cpu in $(all_cpus); do
            echo "$prev_governor" | sudo tee "$(governor_path "$cpu")" >/dev/null 2>&1 || true
        done
    fi
    if [ -e /sys/devices/system/cpu/cpufreq/boost ] && [ "$prev_boost" != "unknown" ]; then
        echo "$prev_boost" | sudo tee /sys/devices/system/cpu/cpufreq/boost >/dev/null 2>&1 || true
    fi
    if [ "$prev_swappiness" != "unknown" ]; then
        echo "$prev_swappiness" | sudo tee /proc/sys/vm/swappiness >/dev/null
    fi

    rm -f "$STATE_FILE"
    echo "Restored governor=$prev_governor boost=$prev_boost swappiness=$prev_swappiness"
}

case "$ACTION" in
    check) do_check ;;
    apply) do_apply ;;
    reset) do_reset ;;
esac
