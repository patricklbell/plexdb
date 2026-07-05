#!/usr/bin/env bash
# run.sh — reproducible benchmark orchestrator: builds, pins, and runs
# cassandra-stress against plexdb and/or a real Cassandra, writes a result
# JSON, and optionally checks it against this machine's baseline.
#
# Usage:
#   ./tools/bench/run.sh [options]
#     -n <ops>              operations per workload                                         (default: 100000)
#     -t <threads>          client threads                                                  (default: 16)
#     -C <cpulist>          cpu list to pin both server and client to                       (default: 0-3)
#     -r <reps>             repetitions per target/workload (first is warm-up, discarded)   (default: 5)
#     -d <dir>              bench scratch/data dir                                          (default: /tmp/plexdb-bench-data)
#     --mode <m>            container|native                                                (default: this machine's baseline mode, else container)
#     --targets <l>         comma-separated: plexdb,cassandra                               (default: plexdb,cassandra)
#     --baseline            compare the result against baseline/<slug>.json, exit non-zero on regression
#     --update-baseline     overwrite baseline/<slug>.json with this run's medians
#     -h                    show this help
#
# See tools/bench/README.md for the full methodology.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

OPS=100000
THREADS=16
CPULIST="0-3"
REPS=5
BENCH_DIR="/tmp/plexdb-bench-data"
MODE=""
TARGETS="plexdb,cassandra"
DO_BASELINE=false
DO_UPDATE_BASELINE=false

while [ $# -gt 0 ]; do
    case "$1" in
        -n) OPS="$2"; shift ;;
        -t) THREADS="$2"; shift ;;
        -C) CPULIST="$2"; shift ;;
        -r) REPS="$2"; shift ;;
        -d) BENCH_DIR="$2"; shift ;;
        --mode) MODE="$2"; shift ;;
        --targets) TARGETS="$2"; shift ;;
        --baseline) DO_BASELINE=true ;;
        --update-baseline) DO_UPDATE_BASELINE=true ;;
        -h)
            sed -n '2,17p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
    shift
done

IFS=',' read -r -a TARGET_LIST <<< "$TARGETS"

RESULTS_DIR="$SCRIPT_DIR/results"
BASELINE_DIR="$SCRIPT_DIR/baseline"
NATIVE_BUILD_DIR="$ROOT_DIR/build-bench"
mkdir -p "$RESULTS_DIR" "$BASELINE_DIR" "$BENCH_DIR"

WORK="$(mktemp -d /tmp/plexdb_bench_run_XXXXXX)"
REPS_DIR="$WORK/reps"
mkdir -p "$REPS_DIR"

PLEXDB_PORT=9042
PLEXDB_PID=""
PLEXDB_CONTAINER="plexdb-bench-plexdb"
PLEXDB_DB="$BENCH_DIR/plexdb"

cleanup() {
    stop_plexdb_container 2>/dev/null || true
    stop_plexdb_native 2>/dev/null || true
    bash "$SCRIPT_DIR/cassandra_server.sh" stop >/dev/null 2>&1 || true
    bash "$SCRIPT_DIR/cassandra_native.sh" stop >/dev/null 2>&1 || true
    [ -n "${DEBUG_KEEP_WORK:-}" ] || rm -rf "$WORK"
}
trap cleanup EXIT

# ── System fingerprint (also used to derive the machine slug) ─────────────
SYSTEM_INFO="$WORK/system_info.json"
bash "$SCRIPT_DIR/system_info.sh" -d "$BENCH_DIR" > "$SYSTEM_INFO"
SLUG="$(python3 -c '
import json, sys
sys.path.insert(0, sys.argv[2])
from compare import slug_of
print(slug_of(json.load(open(sys.argv[1]))))
' "$SYSTEM_INFO" "$SCRIPT_DIR")"
BASELINE_FILE="$BASELINE_DIR/$SLUG.json"

if [ -z "$MODE" ]; then
    if [ -f "$BASELINE_FILE" ]; then
        MODE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("mode","container"))' "$BASELINE_FILE")"
    else
        MODE="container"
    fi
fi
echo "Machine: $SLUG   Mode: $MODE   Targets: $TARGETS"
python3 -c '
import json, sys
path, mode = sys.argv[1], sys.argv[2]
doc = json.load(open(path))
doc["mode"] = mode
json.dump(doc, open(path, "w"), indent=2)
' "$SYSTEM_INFO" "$MODE"

# ── plexdb launcher (mode-aware) ───────────────────────────────────────────
wait_for_port() {
    local port="$1"
    for _ in $(seq 1 60); do
        timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/$port" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

start_plexdb_container() {
    mkdir -p "$PLEXDB_DB"
    docker rm -f "$PLEXDB_CONTAINER" >/dev/null 2>&1 || true
    docker run -d --name "$PLEXDB_CONTAINER" \
        --network host --cpuset-cpus="$CPULIST" \
        --user "$(id -u):$(id -g)" \
        -v "$PLEXDB_DB:/data" \
        plexdb-bench "/data/bench.db" --port "$PLEXDB_PORT" >/dev/null
    wait_for_port "$PLEXDB_PORT" || { echo "error: plexdb container did not open port $PLEXDB_PORT" >&2; exit 1; }
}

stop_plexdb_container() {
    docker rm -f "$PLEXDB_CONTAINER" >/dev/null 2>&1 || true
}

start_plexdb_native() {
    rm -f "$PLEXDB_DB.db" "$PLEXDB_DB.db.wal"
    setarch "$(uname -m)" -R taskset -c "$CPULIST" \
        "$NATIVE_BUILD_DIR/cql/plexdb_cql_server" "$PLEXDB_DB.db" --port "$PLEXDB_PORT" \
        > "$WORK/plexdb_native.log" 2>&1 &
    PLEXDB_PID=$!
    wait_for_port "$PLEXDB_PORT" || { echo "error: native plexdb did not open port $PLEXDB_PORT" >&2; exit 1; }
}

stop_plexdb_native() {
    [ -n "$PLEXDB_PID" ] && kill "$PLEXDB_PID" 2>/dev/null || true
    PLEXDB_PID=""
}

# ── Build ───────────────────────────────────────────────────────────────
if [[ " ${TARGET_LIST[*]} " == *" plexdb "* ]]; then
    if [ "$MODE" = "container" ]; then
        echo "Building plexdb container image..."
        docker build -f "$SCRIPT_DIR/plexdb_container/Dockerfile" -t plexdb-bench "$ROOT_DIR" >/dev/null
    else
        # Native mode intentionally uses whatever toolchain this host has
        # (override with $CXX/$CC) rather than CI's pinned clang-19 — that
        # exact-toolchain guarantee is what container mode is for; native
        # mode's whole point is running with this specific machine's setup.
        echo "Building plexdb natively (CXX=${CXX:-clang++}) into $NATIVE_BUILD_DIR..."
        cmake -B "$NATIVE_BUILD_DIR" -G Ninja -S "$ROOT_DIR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
            -DCMAKE_C_COMPILER="${CC:-clang}" \
            -DCMAKE_CXX_SCAN_FOR_MODULES=ON >/dev/null
        ninja -C "$NATIVE_BUILD_DIR" plexdb_cql_server >/dev/null
    fi
fi

# ── Run one target's write+read workload for REPS repetitions ─────────────
run_target() {
    local target="$1"
    for rep in $(seq 1 "$REPS"); do
        echo "  [$target] rep $rep/$REPS"
        OUT="$WORK/${target}_rep${rep}.txt"
        # cassandra-stress itself can exit non-zero on a successful run (e.g.
        # JMX stats collection failing, which cassandra_stress/run.sh always
        # hits since no JMX server is exposed) — cql-bench.yml already
        # tolerates this with `|| true`. The real success signal is whether
        # parse_stress.py below can find a valid Results block.
        bash "$SCRIPT_DIR/../cassandra_stress/run.sh" \
            -e -p "$PLEXDB_PORT" -C "$CPULIST" -n "$OPS" -t "$THREADS" > "$OUT" 2>&1 || true

        WRITE_OUT="$WORK/${target}_rep${rep}_write.txt"
        READ_OUT="$WORK/${target}_rep${rep}_read.txt"
        awk '/=== Read benchmark/{found=1} !found{print > "'"$WRITE_OUT"'"} found{print > "'"$READ_OUT"'"}' "$OUT"

        python3 "$SCRIPT_DIR/parse_stress.py" "$WRITE_OUT" >> "$REPS_DIR/${target}.write.jsonl"
        python3 "$SCRIPT_DIR/parse_stress.py" "$READ_OUT" >> "$REPS_DIR/${target}.read.jsonl"
    done
}

for target in "${TARGET_LIST[@]}"; do
    echo "=== Target: $target ==="
    case "$target" in
        plexdb)
            PLEXDB_PORT=9042
            if [ "$MODE" = "container" ]; then start_plexdb_container; else start_plexdb_native; fi
            run_target plexdb
            if [ "$MODE" = "container" ]; then stop_plexdb_container; else stop_plexdb_native; fi
            ;;
        cassandra)
            PLEXDB_PORT=9042
            if [ "$MODE" = "container" ]; then
                bash "$SCRIPT_DIR/cassandra_server.sh" start -d "$BENCH_DIR/cassandra" -C "$CPULIST"
            else
                bash "$SCRIPT_DIR/cassandra_native.sh" start -d "$BENCH_DIR/cassandra" -C "$CPULIST"
            fi
            run_target cassandra
            if [ "$MODE" = "container" ]; then
                bash "$SCRIPT_DIR/cassandra_server.sh" stop
            else
                bash "$SCRIPT_DIR/cassandra_native.sh" stop
            fi
            ;;
        *)
            echo "unknown target: $target" >&2
            exit 1
            ;;
    esac
done

# ── Aggregate + write result ───────────────────────────────────────────
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
HOSTNAME_SHORT="$(hostname -s 2>/dev/null || hostname)"
GIT_SHA="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
RESULT_FILE="$RESULTS_DIR/${TIMESTAMP}_${HOSTNAME_SHORT}_${GIT_SHA}.json"

python3 "$SCRIPT_DIR/aggregate.py" "$SYSTEM_INFO" "$REPS_DIR" "$MODE" > "$RESULT_FILE"
echo
echo "Wrote $RESULT_FILE"

if $DO_UPDATE_BASELINE; then
    python3 -c '
import json, sys
result = json.load(open(sys.argv[1]))
baseline = {
    "slug": sys.argv[3],
    "hostname": result["host"]["hostname"],
    "mode": result["mode"],
    "generated": result["timestamp"],
    "targets": result["targets"],
}
json.dump(baseline, open(sys.argv[2], "w"), indent=2)
print()
' "$RESULT_FILE" "$BASELINE_FILE" "$SLUG"
    echo "Updated $BASELINE_FILE"
fi

if $DO_BASELINE; then
    if [ ! -f "$BASELINE_FILE" ]; then
        echo "error: no baseline at $BASELINE_FILE — run with --update-baseline first" >&2
        exit 1
    fi
    python3 "$SCRIPT_DIR/compare.py" "$RESULT_FILE" "$BASELINE_FILE"
fi
