#!/usr/bin/env bash
# run.sh — fetch redis-py conformance tests and run them against resp_server.
#
# The upstream tests come from redis/redis-py's tests/test_commands.py.
# A local conftest.py provides server lifecycle, the `r` fixture, and the
# skip-helper functions that test_commands.py imports from tests.conftest.
#
# Usage:
#   ./extra/resp_tests/run.sh [options]
#     -b <path>    resp server binary                   (default: ./build/resp/plexdb_resp_server)
#     -P <port>    TCP port                             (default: 6399)
#     -r <ref>     redis-py git ref/tag                 (default: v4.6.0)
#     -t <expr>    pytest -k expression                 (default: run all)
#     -x           stop on first failure                (pytest -x)
#     -l           list tests only                      (pytest --collect-only)
#     -L <dir>     write server log to <dir>/server.log
#     -h           show this help
#
# Environment:
#   RESP_TEST_HOST    override host (default: 127.0.0.1)
#   RESP_TEST_PORT    override port (default: value of -P)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY="${BINARY:-$ROOT_DIR/build/resp/resp_server}"
PORT="${PORT:-6399}"
REDIS_PY_REF="${REDIS_PY_REF:-v4.6.0}"
PYTEST_K=""
PYTEST_EXTRA=""
LIST_ONLY=false
LOG_DIR=""

while getopts "b:P:r:t:xlL:h" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        r) REDIS_PY_REF="$OPTARG" ;;
        t) PYTEST_K="$OPTARG" ;;
        x) PYTEST_EXTRA="$PYTEST_EXTRA -x" ;;
        l) LIST_ONLY=true ;;
        L) LOG_DIR="$OPTARG" ;;
        h)
            sed -n '2,22p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

HOST="${RESP_TEST_HOST:-127.0.0.1}"
PORT="${RESP_TEST_PORT:-$PORT}"

# ── Fetch redis-py tests ──────────────────────────────────────────────────
# The tests are placed in redis_tests/tests/ so that the package structure
# satisfies "from tests.conftest import ..." imports in test_commands.py.
TESTS_DIR="$SCRIPT_DIR/redis_tests"
STAMP="$TESTS_DIR/.ref"

fetch_tests() {
    local ref="$1"
    local base_url="https://raw.githubusercontent.com/redis/redis-py/${ref}"

    echo "Fetching redis-py tests (ref: ${ref})..."
    rm -rf "$TESTS_DIR"
    mkdir -p "$TESTS_DIR/tests"

    # test_commands.py is the main command-level conformance file in redis-py 4.x
    curl -sfL "${base_url}/tests/test_commands.py" \
        -o "$TESTS_DIR/tests/test_commands.py" || {
        echo "Warning: could not fetch test_commands.py from redis-py ${ref}"
        touch "$TESTS_DIR/tests/test_commands.py"
    }

    # __init__.py so the directory is importable as the 'tests' package
    touch "$TESTS_DIR/__init__.py"
    touch "$TESTS_DIR/tests/__init__.py"

    echo "$ref" > "$STAMP"
    echo "Tests fetched."
}

if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$REDIS_PY_REF" ]; then
    fetch_tests "$REDIS_PY_REF"
else
    echo "Using cached redis-py tests (ref: $REDIS_PY_REF)"
fi

# Always refresh our local files into the test package
cp "$SCRIPT_DIR/conftest.py"      "$TESTS_DIR/tests/conftest.py"
cp "$SCRIPT_DIR/test_benchmark.py" "$TESTS_DIR/tests/test_benchmark.py"
echo "Installed shims."

# ── Python venv ───────────────────────────────────────────────────────────
VENV="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV" ]; then
    echo "Creating venv with Python 3.12 via uv..."
    uv venv --python 3.12 "$VENV"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"
# Pin redis-py to the same version as the fetched test files so internal
# imports (redis.client symbols, etc.) match exactly.
REDIS_PY_VERSION="${REDIS_PY_REF#v}"   # strip leading 'v' if present
uv pip install -q "redis==${REDIS_PY_VERSION}" pytest

# ── List mode ─────────────────────────────────────────────────────────────
if $LIST_ONLY; then
    echo ""
    python3 -m pytest "$TESTS_DIR/tests/" \
        --collect-only -q \
        --override-ini="pythonpath=$TESTS_DIR" \
        --rootdir="$TESTS_DIR/tests" \
        --confcutdir="$TESTS_DIR/tests" \
        ${PYTEST_K:+-k "$PYTEST_K"} \
        2>&1 || true
    exit 0
fi

# ── Run ───────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════════"
echo "  RESP Conformance Tests (redis-py ref: ${REDIS_PY_REF})"
echo "  Server: ${HOST}:${PORT}"
if [ -n "$LOG_DIR" ]; then
echo "  Logs:   ${LOG_DIR}/server.log"
fi
echo "════════════════════════════════════════════════════════════"
echo ""

python3 -m pytest "$TESTS_DIR/tests/" \
    --host "$HOST" --port "$PORT" --binary "$BINARY" \
    ${LOG_DIR:+--log-dir "$LOG_DIR"} \
    --override-ini="pythonpath=$TESTS_DIR" \
    --rootdir="$TESTS_DIR/tests" \
    --confcutdir="$TESTS_DIR/tests" \
    -p no:cacheprovider \
    -v --tb=short \
    ${PYTEST_K:+-k "$PYTEST_K"} \
    $PYTEST_EXTRA
