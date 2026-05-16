#!/bin/bash
PIDFILE="${BASH_SOURCE%/*}/../out/plexdb_cql_server.pid"

echo $PIDFILE
if [ ! -f "$PIDFILE" ]; then
    echo "plexdb_cql_server is not running (no pidfile)"
    exit 1
fi

pid=$(cat "$PIDFILE")

if ! kill -0 "$pid" 2>/dev/null; then
    echo "stale pidfile, process $pid is not running"
    rm "$PIDFILE"
    exit 1
fi

echo "Sending USR1 to pid $pid..."
kill -SIGUSR1 "$pid"
