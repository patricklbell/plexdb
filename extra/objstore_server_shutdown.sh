#!/bin/bash
PIDFILE="${BASH_SOURCE%/*}/../out/objstore.pid"

echo $PIDFILE
if [ ! -f "$PIDFILE" ]; then
    echo "objstore is not running (no pidfile)"
    exit 1
fi

pid=$(cat "$PIDFILE")

if ! kill -0 "$pid" 2>/dev/null; then
    echo "stale pidfile, process $pid is not running"
    rm "$PIDFILE"
    exit 1
fi

echo "Sending graceful shutdown to pid $pid..."
kill -SIGUSR1 "$pid"