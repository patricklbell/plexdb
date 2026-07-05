#!/usr/bin/env bash
# cassandra_server.sh — start/stop a real Cassandra server in a container, pinned.
#
# Runs the official `cassandra:5.0` image with --network host so it binds
# directly to the host's port 9042, the same port plexdb uses — safe
# because the bench orchestrator (run.sh) always runs targets sequentially,
# never concurrently.
#
# Usage:
#   ./tools/bench/cassandra_server.sh start -d <datadir> [-C <cpulist>] [-H <heap>]
#   ./tools/bench/cassandra_server.sh stop
#     -d <dir>      data dir to bind-mount as Cassandra's data directory  (required for start)
#     -C <cpulist>  taskset-style cpu list, passed to --cpuset-cpus       (default: none, unpinned)
#     -H <heap>     -e MAX_HEAP_SIZE for the JVM                         (default: 4G)
#     -h            show this help

set -euo pipefail

CONTAINER_NAME="plexdb-bench-cassandra"
IMAGE="cassandra:5.0"
ACTION="${1:-}"
[ $# -gt 0 ] && shift

DATA_DIR=""
CPULIST=""
HEAP="4G"

while getopts "d:C:H:h" opt; do
    case "$opt" in
        d) DATA_DIR="$OPTARG" ;;
        C) CPULIST="$OPTARG" ;;
        H) HEAP="$OPTARG" ;;
        h)
            sed -n '2,15p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

case "$ACTION" in
    start)
        if [ -z "$DATA_DIR" ]; then
            echo "error: -d <datadir> is required for start" >&2
            exit 1
        fi
        mkdir -p "$DATA_DIR"

        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true

        CPU_ARGS=()
        [ -n "$CPULIST" ] && CPU_ARGS=(--cpuset-cpus="$CPULIST")

        echo "Starting $IMAGE (heap=$HEAP, cpus=${CPULIST:-unpinned})..."
        docker run -d --name "$CONTAINER_NAME" \
            --network host \
            "${CPU_ARGS[@]}" \
            --user "$(id -u):$(id -g)" \
            -e HOME=/tmp \
            -e MAX_HEAP_SIZE="$HEAP" \
            -e HEAP_NEWSIZE=800M \
            -v "$DATA_DIR:/var/lib/cassandra" \
            "$IMAGE" >/dev/null

        echo "Waiting for Cassandra to accept CQL connections..."
        for _ in $(seq 1 120); do
            if docker exec "$CONTAINER_NAME" cqlsh -e "SELECT release_version FROM system.local;" >/dev/null 2>&1; then
                echo "Cassandra ready."
                exit 0
            fi
            sleep 2
        done

        echo "error: Cassandra did not become ready within 240s" >&2
        docker logs --tail 50 "$CONTAINER_NAME" >&2 || true
        exit 1
        ;;
    stop)
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
        echo "Stopped."
        ;;
    *)
        echo "usage: $0 {start|stop} [options] (-h for help)" >&2
        exit 1
        ;;
esac
