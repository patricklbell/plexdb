#!/usr/bin/env bash
# run cassandra-stress against a local cql instance
#
# Usage: [options]
#   -b <path>    plexdb_cql_server binary (default: ./build/cql/plexdb_cql_server)
#   -n <ops>     operations per workload  (default: 100000)
#   -t <threads> client threads           (default: 16)
#   -p <port>    native protocol port     (default: 9042)
#   -P <port>    LD_PRELOAD for binary    (default: )
#   -C <cpulist> pin server (taskset) and stress client (--cpuset-cpus) to this cpu list (default: unpinned)
#   -e           external server: don't start $BINARY, target an already-running -node/-port
#   -h           show this help

set -euo pipefail

LD_PRELOAD=""

BINARY="${BINARY:-./build/cql/plexdb_cql_server}"
OPS="${OPS:-100000}"
THREADS="${THREADS:-16}"
PORT="${PORT:-9042}"
PRELOAD=""
CPULIST=""
EXTERNAL=false

while getopts "b:n:t:p:P:C:eh" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        n) OPS="$OPTARG" ;;
        t) THREADS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        P) PRELOAD="$OPTARG" ;;
        C) CPULIST="$OPTARG" ;;
        e) EXTERNAL=true ;;
        h)
            sed -n '2,13p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not found — required to run scylladb/cassandra-stress" >&2
    echo "Install Docker: https://docs.docker.com/engine/install/" >&2
    exit 1
fi

DB=""
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "$DB" ] && rm -f "$DB"
}
trap cleanup EXIT

if $EXTERNAL; then
    echo "Targeting external server on port $PORT..."
else
    if [ ! -f "$BINARY" ]; then
        echo "error: cql binary not found at '$BINARY'" >&2
        exit 1
    fi

    DB="$(mktemp -t bench_cql_XXXXXX.db)"

    echo "Starting cql on port $PORT..."
    if [ -n "$PRELOAD" ]; then
        echo "Setting LD_PRELOAD=${PRELOAD}"
    fi
    LAUNCH=("$BINARY" "$DB" --port "$PORT")
    if [ -n "$CPULIST" ]; then
        LAUNCH=(taskset -c "$CPULIST" "${LAUNCH[@]}")
    fi
    LD_PRELOAD=${PRELOAD} "${LAUNCH[@]}" &
    LD_PRELOAD=""
    SERVER_PID=$!
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="cassandra-stress-apache"

CLIENT_CPU_ARGS=()
[ -n "$CPULIST" ] && CLIENT_CPU_ARGS=(--cpuset-cpus="$CPULIST")

echo
echo "=== Building cassandra-stress image ==="
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

echo
echo "=== Write benchmark (n=$OPS, threads=$THREADS) ==="
docker run --rm --network host "${CLIENT_CPU_ARGS[@]}" "$IMAGE_NAME" \
  cassandra-stress write n="$OPS" -rate threads="$THREADS" -node 127.0.0.1 -port native="$PORT"

echo
echo "=== Read benchmark (n=$OPS, threads=$THREADS) ==="
docker run --rm --network host "${CLIENT_CPU_ARGS[@]}" "$IMAGE_NAME" \
  cassandra-stress read n="$OPS" -rate threads="$THREADS" -node 127.0.0.1 -port native="$PORT"
