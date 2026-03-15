#!/usr/bin/env bash
# bench-cassandra-stress.sh — run cassandra-stress against a local objstore instance
#
# Usage: ./extra/bench-cassandra-stress.sh [options]
#   -b <path>   objstore_server binary  (default: ./build/objstore/objstore_server)
#   -n <ops>    operations per workload (default: 100000)
#   -t <threads> client threads         (default: 16)
#   -p <port>   native protocol port    (default: 9042)
#   -h          show this help

set -euo pipefail

BINARY="${BINARY:-./build/objstore/objstore_server}"
OPS="${OPS:-100000}"
THREADS="${THREADS:-16}"
PORT="${PORT:-9042}"

while getopts "b:n:t:p:h" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        n) OPS="$OPTARG" ;;
        t) THREADS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
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
    echo "error: objstore binary not found at '$BINARY'" >&2
    echo "Build with:" >&2
    echo "  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\" >&2
    echo "    -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_C_COMPILER=clang-19 \\" >&2
    echo "    -DPLEXDB_DEBUG=OFF -DBUILD_TESTS=OFF" >&2
    echo "  ninja -C build" >&2
    exit 1
fi

DB="$(mktemp -t bench_objstore_XXXXXX.db)"
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -f "$DB"
}
trap cleanup EXIT

echo "Starting objstore on port $PORT (native mode)..."
"$BINARY" "$DB" --native --port "$PORT" &
SERVER_PID=$!

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
echo "=== Write benchmark (n=$OPS, threads=$THREADS) ==="
docker run --rm --network host scylladb/cassandra-stress write n="$OPS" \
    -rate threads="$THREADS" \
    -node 127.0.0.1 -port native="$PORT"

echo
echo "=== Read benchmark (n=$OPS, threads=$THREADS) ==="
docker run --rm --network host scylladb/cassandra-stress read n="$OPS" \
    -rate threads="$THREADS" \
    -node 127.0.0.1 -port native="$PORT"
