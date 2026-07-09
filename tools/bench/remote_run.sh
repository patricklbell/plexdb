#!/usr/bin/env bash
# remote_run.sh — run the bench orchestrator on another machine over ssh.
#
# Deliberately thin: no daemon, no registration step. The target just needs
# to be reachable over ssh with docker and the build toolchain already
# installed, and a checkout of this repo at the given path (default ~/.cache/plexdb).
# Results come back automatically named with that machine's hostname (see
# run.sh), so there's no collision with local results.
#
# Usage:
#   ./tools/bench/remote_run.sh <user@host> [-p <remote_path>] [-- <run.sh args...>]
#     -p <path>    repo checkout path on the remote host   (default: ~/.cache/plexdb)
#     -h           show this help
#
# If the local working tree has uncommitted changes, they're rsynced to the
# remote checkout so what's benchmarked matches what's actually on disk here;
# otherwise the remote checkout is just fetched and checked out to the local
# HEAD commit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [ $# -eq 0 ] || [ "$1" = "-h" ]; then
    sed -n '2,16p' "$0" | sed 's/^# \?//'
    exit 0
fi

HOST="$1"
shift

REMOTE_PATH="~/.cache/plexdb"
while [ $# -gt 0 ]; do
    case "$1" in
        -p) REMOTE_PATH="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
RUN_ARGS=("$@")

echo "=== Syncing repo to $HOST:$REMOTE_PATH ==="
if git -C "$ROOT_DIR" diff --quiet && git -C "$ROOT_DIR" diff --cached --quiet; then
    ORIGIN_URL="$(git -C "$ROOT_DIR" remote get-url origin 2>/dev/null || true)"
    if [ -z "$ORIGIN_URL" ]; then
        echo "error: no 'origin' remote configured locally — can't fetch on $HOST" >&2
        echo "either add a remote, or make an uncommitted change so this falls back to rsync" >&2
        exit 1
    fi
    LOCAL_SHA="$(git -C "$ROOT_DIR" rev-parse HEAD)"
    ssh "$HOST" "mkdir -p $REMOTE_PATH && cd $REMOTE_PATH && \
        (git rev-parse --is-inside-work-tree >/dev/null 2>&1 || git init -q .) && \
        git fetch -q $ORIGIN_URL $LOCAL_SHA && \
        git checkout -q $LOCAL_SHA"
else
    echo "Local working tree has uncommitted changes — rsyncing instead of git checkout."
    rsync -az --delete \
        --exclude='.git/' --exclude='build/' --exclude='out/' --exclude='.cache/' \
        --exclude='tools/bench/results/' \
        "$ROOT_DIR/" "$HOST:$REMOTE_PATH/"
fi

echo "=== Running tools/bench/run.sh on $HOST ==="
ssh "$HOST" "cd $REMOTE_PATH && tools/bench/run.sh ${RUN_ARGS[*]}"

echo "=== Pulling results back ==="
mkdir -p "$SCRIPT_DIR/results"
rsync -az "$HOST:$REMOTE_PATH/tools/bench/results/" "$SCRIPT_DIR/results/"
echo "Done — see tools/bench/results/"
