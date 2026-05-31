#!/usr/bin/env bash
# run.sh — fetch redis-py conformance tests and run them against keyvalue_server.
#
# The upstream tests come from redis/redis-py's tests/test_commands.py.
# A local conftest.py provides server lifecycle, the `r` fixture, and the
# skip-helper functions that test_commands.py imports from tests.conftest.
#
# Usage:
#   ./extra/resp_tests/run.sh [options]
#     -b <path>    resp server binary                   (default: ./build/keyvalue/plexdb_keyvalue_server)
#     -p <path>    plugin .so                           (default: empty)
#     -d <path>    database file path (persistent mode) (default: in-memory)
#     -P <port>    TCP port                             (default: 6399)
#     -r <ref>     redis-py git ref/tag                 (default: v4.6.0)
#     -t <expr>    pytest -k expression                 (default: run all)
#     -x           stop on first failure                (pytest -x)
#     -l           list tests only                      (pytest --collect-only)
#     -L <dir>     write server log to <dir>/server.log
#     -m <file>    fail if a listed test regresses      (default: none)
#     -o <file>    write passing IDs to file (gen mustpass) (default: none)
#     -h           show this help
#
# Environment:
#   RESP_TEST_HOST    override host (default: 127.0.0.1)
#   RESP_TEST_PORT    override port (default: value of -P)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY="${BINARY:-$ROOT_DIR/build/keyvalue/plexdb_keyvalue_server}"
PLUGINS=()
DB_PATH=""
PORT="${PORT:-6399}"
REDIS_PY_REF="${REDIS_PY_REF:-v4.6.0}"
PYTEST_K=""
PYTEST_EXTRA=""
LIST_ONLY=false
LOG_DIR=""
MUSTPASS_FILE=""
OUTPUT_MUSTPASS=""

while getopts "b:p:d:P:r:t:m:o:xlL:h" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        p) PLUGINS+=("$OPTARG") ;;
        d) DB_PATH="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        r) REDIS_PY_REF="$OPTARG" ;;
        t) PYTEST_K="$OPTARG" ;;
        m) MUSTPASS_FILE="$OPTARG" ;;
        o) OUTPUT_MUSTPASS="$OPTARG" ;;
        x) PYTEST_EXTRA="$PYTEST_EXTRA -x" ;;
        l) LIST_ONLY=true ;;
        L) LOG_DIR="$OPTARG" ;;
        h)
            sed -n '2,26p' "$0" | sed 's/^# \?//'
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

PLUGIN_ARGS=()
for p in "${PLUGINS[@]}"; do
    PLUGIN_ARGS+=(--plugin "$p")
done

run_pytest() {
    python3 -m pytest "$TESTS_DIR/tests/" \
        --host "$HOST" --port "$PORT" --binary "$BINARY" \
        "${PLUGIN_ARGS[@]}" \
        ${DB_PATH:+--db-path "$DB_PATH"} \
        ${LOG_DIR:+--log-dir "$LOG_DIR"} \
        --override-ini="pythonpath=$TESTS_DIR" \
        --rootdir="$TESTS_DIR/tests" \
        --confcutdir="$TESTS_DIR/tests" \
        -p no:cacheprovider \
        -v --tb=short \
        ${PYTEST_K:+-k "$PYTEST_K"} \
        $PYTEST_EXTRA
}

if [ -n "$MUSTPASS_FILE" ] || [ -n "$OUTPUT_MUSTPASS" ]; then
    MUSTPASS_TMP="$(mktemp /tmp/plexdb_resp_pytest_XXXXXX.txt)"
    set +e
    run_pytest 2>&1 | tee "$MUSTPASS_TMP"
    set -e
    python3 - "$MUSTPASS_TMP" "${OUTPUT_MUSTPASS}" "${MUSTPASS_FILE}" <<'PYEOF'
import sys, re
from datetime import datetime

def strip_ansi(s):
    return re.sub(r'\x1b\[[0-9;]*[mGKHF]', '', s)

output_file = sys.argv[1]
output_mustpass = sys.argv[2] if sys.argv[2] else None
mustpass_file  = sys.argv[3] if sys.argv[3] else None

results = {}
with open(output_file) as f:
    for line in f:
        line = strip_ansi(line).rstrip()
        m = re.match(
            r'^(.+?)\s+(PASSED|FAILED|ERROR|SKIPPED|XFAIL|XPASS|XFAILED|XPASSED)'
            r'(?:\s+\[[\s\d%]+\])?\s*$', line)
        if m:
            results[m.group(1).strip()] = m.group(2)

exit_code = 0

if output_mustpass:
    passing = sorted(nid for nid, st in results.items() if st == 'PASSED')
    with open(output_mustpass, 'w') as f:
        f.write('# Mustpass list — tests that must pass to prevent regressions.\n')
        f.write('# Generated: ' + datetime.now().strftime('%Y-%m-%d') + '\n')
        f.write('# Regenerate: tools/resp_tests/run.sh -o tools/resp_tests/mustpass.txt\n#\n')
        for nid in passing:
            f.write(nid + '\n')
    print(f'Wrote {len(passing)} passing tests to {output_mustpass}')

if mustpass_file:
    try:
        with open(mustpass_file) as f:
            mustpass = [l.strip() for l in f if l.strip() and not l.startswith('#')]
    except FileNotFoundError:
        print(f'Warning: mustpass file not found: {mustpass_file}', file=sys.stderr)
        mustpass = []

    regressions = [(nid, results[nid]) for nid in mustpass if results.get(nid) in ('FAILED', 'ERROR')]

    if regressions:
        print(f'\nMUSTPASS REGRESSIONS — {len(regressions)} test(s) that must pass have failed:')
        for nid, st in regressions:
            print(f'  {st}: {nid}')
        exit_code = 1
    else:
        checked = sum(1 for nid in mustpass if nid in results)
        print(f'\nMustpass OK ({checked}/{len(mustpass)} verified in this run)')

sys.exit(exit_code)
PYEOF
    MUSTPASS_EXIT=$?
    rm -f "$MUSTPASS_TMP"
    exit $MUSTPASS_EXIT
else
    run_pytest
fi
