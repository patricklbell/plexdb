#!/usr/bin/env bash
# cassandra_native.sh — fetch, configure and launch Cassandra natively (native-mode fallback).
#
# Same start/stop interface as cassandra_server.sh, for machines where the
# container-overhead sanity check (see README) says containerizing plexdb
# isn't reliable enough, so the real Cassandra target has to run natively
# too rather than dockerized.
#
# Downloads/caches the same Cassandra release already pinned in
# tools/cassandra_stress/Dockerfile, under ~/.cache/plexdb-bench/.
#
# Usage:
#   ./tools/bench/cassandra_native.sh start -d <datadir> [-C <cpulist>] [-H <heap>]
#   ./tools/bench/cassandra_native.sh stop
#     -d <dir>      data dir for Cassandra's data/commitlog/etc.  (required for start)
#     -C <cpulist>  taskset -c cpu list to pin the JVM to          (default: none, unpinned)
#     -H <heap>     MAX_HEAP_SIZE for the JVM                     (default: 4G)
#     -h            show this help
#
# Environment:
#   CASSANDRA_JAVA_HOME   JDK to run Cassandra with, if the default `java`
#                         on PATH isn't a supported major version (11 or 17).

set -euo pipefail

VERSION="5.0.8"
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/plexdb-bench"
INSTALL_DIR="$CACHE_DIR/apache-cassandra-${VERSION}"
PID_FILE="$CACHE_DIR/cassandra_native.pid"

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
            sed -n '2,20p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

resolve_java() {
    local java_bin="${CASSANDRA_JAVA_HOME:+$CASSANDRA_JAVA_HOME/bin/}java"
    java_bin="${java_bin:-java}"
    if ! command -v "$java_bin" >/dev/null 2>&1; then
        echo "error: no java found (looked for '$java_bin')" >&2
        exit 1
    fi
    local major
    major="$("$java_bin" -version 2>&1 | head -1 | sed -E 's/.*"([0-9]+).*/\1/')"
    case "$major" in
        11|17) : ;;
        *)
            echo "error: Cassandra ${VERSION} supports Java 11 or 17; found major version $major ($java_bin)." >&2
            echo "Install a compatible JDK and set CASSANDRA_JAVA_HOME, e.g.:" >&2
            echo "  sudo dnf install java-17-openjdk && CASSANDRA_JAVA_HOME=/usr/lib/jvm/java-17-openjdk tools/bench/cassandra_native.sh start ..." >&2
            exit 1
            ;;
    esac
    echo "$java_bin"
}

fetch_cassandra() {
    if [ -x "$INSTALL_DIR/bin/cassandra" ]; then
        return
    fi
    mkdir -p "$CACHE_DIR"
    echo "Fetching Apache Cassandra ${VERSION}..."
    curl -sfL "https://downloads.apache.org/cassandra/${VERSION}/apache-cassandra-${VERSION}-bin.tar.gz" \
        -o "$CACHE_DIR/apache-cassandra-${VERSION}-bin.tar.gz"
    tar -xzf "$CACHE_DIR/apache-cassandra-${VERSION}-bin.tar.gz" -C "$CACHE_DIR"
}

case "$ACTION" in
    start)
        if [ -z "$DATA_DIR" ]; then
            echo "error: -d <datadir> is required for start" >&2
            exit 1
        fi
        if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
            echo "error: already running (pid $(cat "$PID_FILE")) — run stop first" >&2
            exit 1
        fi

        JAVA_BIN="$(resolve_java)"
        fetch_cassandra

        mkdir -p "$DATA_DIR"/{data,commitlog,saved_caches,hints}
        rm -rf "$DATA_DIR/conf"
        cp -r "$INSTALL_DIR/conf" "$DATA_DIR/conf"

        python3 - "$DATA_DIR/conf/cassandra.yaml" "$DATA_DIR" <<'PYEOF'
import sys
path, data_dir = sys.argv[1], sys.argv[2]
with open(path) as f:
    text = f.read()
import re
text = re.sub(r'(?m)^data_file_directories:\n(?:    - .*\n)+',
              f'data_file_directories:\n    - {data_dir}/data\n', text)
text = re.sub(r'(?m)^commitlog_directory: .*$', f'commitlog_directory: {data_dir}/commitlog', text)
text = re.sub(r'(?m)^saved_caches_directory: .*$', f'saved_caches_directory: {data_dir}/saved_caches', text)
text = re.sub(r'(?m)^hints_directory: .*$', f'hints_directory: {data_dir}/hints', text)
with open(path, 'w') as f:
    f.write(text)
PYEOF

        export CASSANDRA_CONF="$DATA_DIR/conf"
        export MAX_HEAP_SIZE="$HEAP"
        export HEAP_NEWSIZE=800M
        export JAVA_HOME="${CASSANDRA_JAVA_HOME:-${JAVA_HOME:-}}"

        LAUNCH=("$INSTALL_DIR/bin/cassandra" -f)
        if [ -n "$CPULIST" ]; then
            LAUNCH=(taskset -c "$CPULIST" "${LAUNCH[@]}")
        fi

        echo "Starting native Cassandra (java=$JAVA_BIN, heap=$HEAP, cpus=${CPULIST:-unpinned})..."
        "${LAUNCH[@]}" > "$DATA_DIR/cassandra.log" 2>&1 &
        echo $! > "$PID_FILE"

        echo "Waiting for Cassandra to accept CQL connections..."
        # Not using cqlsh here: it's a bundled Python script pinned to
        # Python 3.6-3.11, and refuses to run under a newer host python3
        # (e.g. Fedora ships 3.14) — "Startup complete" in the log plus a
        # raw TCP connect to the native port is what the daemon itself
        # considers ready, and has no host-python dependency.
        for _ in $(seq 1 120); do
            if grep -q "Startup complete" "$DATA_DIR/cassandra.log" 2>/dev/null \
                && timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/9042" 2>/dev/null; then
                echo "Cassandra ready."
                exit 0
            fi
            sleep 2
        done

        echo "error: Cassandra did not become ready within 240s — see $DATA_DIR/cassandra.log" >&2
        tail -n 50 "$DATA_DIR/cassandra.log" >&2 || true
        exit 1
        ;;
    stop)
        if [ -f "$PID_FILE" ]; then
            PID="$(cat "$PID_FILE")"
            if kill -0 "$PID" 2>/dev/null; then
                kill -TERM "$PID" 2>/dev/null || true
                for _ in $(seq 1 30); do
                    kill -0 "$PID" 2>/dev/null || break
                    sleep 1
                done
                kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null || true
            fi
            rm -f "$PID_FILE"
        fi
        echo "Stopped."
        ;;
    *)
        echo "usage: $0 {start|stop} [options] (-h for help)" >&2
        exit 1
        ;;
esac
