#!/bin/bash
PIDDIR="${BASH_SOURCE%/*}/../out"

shopt -s nullglob
pidfiles=("$PIDDIR"/*.pid)

if [ ${#pidfiles[@]} -eq 0 ]; then
    echo "no pid files found in $PIDDIR"
    exit 1
fi

for PIDFILE in "${pidfiles[@]}"; do
    pid=$(cat "$PIDFILE")

    if ! kill -0 "$pid" 2>/dev/null; then
        echo "stale pidfile $PIDFILE, process $pid is not running"
        rm "$PIDFILE"
        continue
    fi

    echo "Sending USR1 to pid $pid ($PIDFILE)..."
    kill -SIGUSR1 "$pid"
done
