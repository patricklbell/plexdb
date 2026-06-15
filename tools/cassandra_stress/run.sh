#!/usr/bin/env bash
# run cassandra-stress against a local cql instance
#
# Usage: [options]
#   -b <path>    plexdb_cql_server binary (default: ./build/debug/cql/plexdb_cql_server)
#   -n <ops>     operations per workload  (default: 100000)
#   -t <threads> client threads           (default: 16)
#   -p <port>    native protocol port     (default: 9042)
#   -P <port>    LD_PRELOAD for binary    (default: )
#   -h           show this help

set -euo pipefail

LD_PRELOAD=""

BINARY="${BINARY:-./build/debug/cql/plexdb_cql_server}"
OPS="${OPS:-100000}"
THREADS="${THREADS:-16}"
PORT="${PORT:-9042}"
PRELOAD=""

while getopts "b:n:t:p:P:h" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        n) OPS="$OPTARG" ;;
        t) THREADS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        P) PRELOAD="$OPTARG" ;;
        h)
            sed -n '2,9p' "$0" | sed 's/^# \?//'
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

if [ ! -f "$BINARY" ]; then
    echo "error: cql binary not found at '$BINARY'" >&2
    exit 1
fi

DB="$(mktemp -t bench_cql_XXXXXX.db)"
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$DB"
}
trap cleanup EXIT

echo "Starting cql on port $PORT..."
if [ -n "$PRELOAD" ]; then
    echo "Setting LD_PRELOAD=${PRELOAD}"
fi
LD_PRELOAD=${PRELOAD} "$BINARY" "$DB" --port "$PORT" &
LD_PRELOAD=""
SERVER_PID=$!

echo "Checking server"

python3 - "$PORT" <<'EOF'
import socket, time, sys
port = int(sys.argv[1])
for _ in range(100):
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=1)
        s.close()
        sys.exit(0)
    except OSError:
        time.sleep(0.2)
print("error: server did not become ready", file=sys.stderr)
sys.exit(1)
EOF
echo

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="cassandra-stress-apache"

echo
echo "=== Building cassandra-stress image ==="
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

echo
echo "=== Write benchmark (n=$OPS, threads=$THREADS) ==="
docker run --rm --network host "$IMAGE_NAME" \
  cassandra-stress write n="$OPS" -rate threads="$THREADS" -node 127.0.0.1 -port native="$PORT"

echo
echo "=== Read benchmark (n=$OPS, threads=$THREADS) ==="
docker run --rm --network host "$IMAGE_NAME" \
  cassandra-stress read n="$OPS" -rate threads="$THREADS" -node 127.0.0.1 -port native="$PORT"
