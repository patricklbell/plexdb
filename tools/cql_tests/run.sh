#!/usr/bin/env bash
# run.sh — fetch ScyllaDB CQL tests and run them against a local cql instance.
#
# The tests come from scylladb/scylladb's test/cqlpy/cassandra_tests/ directory,
# which contains Apache Cassandra CQL tests ported to Python+pytest. Only a
# compatibility shim (conftest.py, porting.py, util.py) is local; the actual
# test content is fetched verbatim from the upstream repository.
#
# Usage:
#   ./extra/cql_tests/run.sh [options]
#     -b <path>    plexdb_cql_server binary                          (default: ./build/cql/plexdb_cql_server)
#     -p <path>    plugin .so                                        (default: empty)
#     -P <port>    native protocol port                              (default: 9042)
#     -r <ref>     scylladb git ref/tag                              (default: master)
#     -t <expr>    pytest -k expression                              (default: run all)
#     -x           stop on first failure                             (pytest -x)
#     -l           list tests only                                   (pytest --collect-only)
#     -L <dir>     write per-session server log to <dir>/server.log
#     -m <file>    fail if a listed test regresses                   (default: none)
#     -o <file>    write passing test IDs to <file> (gen mustpass)   (default: none)
#     -s <file>    skiplist (deselect permissive-parser tests)       (default: tools/cql_tests/skiplist.txt)
#     -h           show this help
#
# Environment:
#   CQL_TEST_HOST    override contact point (default: 127.0.0.1)
#   CQL_TEST_PORT    override port          (default: value of -P)
#   SCYLLA_REF       same as -r

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY="${BINARY:-$ROOT_DIR/build/cql/plexdb_cql_server}"
PLUGINS=()
PORT="${PORT:-9042}"
SCYLLA_REF="${SCYLLA_REF:-master}"
PYTEST_K=""
PYTEST_EXTRA=""
LIST_ONLY=false
LOG_DIR=""
MUSTPASS_FILE=""
OUTPUT_MUSTPASS=""
SKIPLIST_FILE="$SCRIPT_DIR/skiplist.txt"

VALGRIND=false
while getopts "b:p:P:r:t:m:o:s:xlL:Vh" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        p) PLUGINS+=("$OPTARG") ;;
        P) PORT="$OPTARG" ;;
        r) SCYLLA_REF="$OPTARG" ;;
        t) PYTEST_K="$OPTARG" ;;
        m) MUSTPASS_FILE="$OPTARG" ;;
        o) OUTPUT_MUSTPASS="$OPTARG" ;;
        s) SKIPLIST_FILE="$OPTARG" ;;
        x) PYTEST_EXTRA="$PYTEST_EXTRA -x" ;;
        l) LIST_ONLY=true ;;
        L) LOG_DIR="$OPTARG" ;;
        V) VALGRIND=true ;;
        h)
            sed -n '2,27p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

HOST="${CQL_TEST_HOST:-127.0.0.1}"
PORT="${CQL_TEST_PORT:-$PORT}"

# ── Valgrind (memcheck) ───────────────────────────────────────────────────
# -V wraps the server in valgrind. The harness restarts the server on a detected
# crash, so each server process logs to its own vg.<pid>.log; after the run we scan
# them all and fail if any real error remains (known false positives are suppressed).
VG_LOG_DIR=""
if [ "$VALGRIND" = true ]; then
    command -v valgrind >/dev/null || { echo "error: -V requires valgrind on PATH" >&2; exit 1; }
    VG_LOG_DIR="$(mktemp -d /tmp/plexdb_cql_vg_XXXXXX)"
    VG_WRAPPER="$(mktemp /tmp/plexdb_cql_vgwrap_XXXXXX.sh)"
    cat > "$VG_WRAPPER" <<EOF
#!/bin/bash
exec valgrind \\
    --error-exitcode=0 \\
    --track-origins=yes \\
    --num-callers=25 \\
    --suppressions="$SCRIPT_DIR/valgrind.supp" \\
    --log-file="$VG_LOG_DIR/vg.%p.log" \\
    "$BINARY" "\$@"
EOF
    chmod +x "$VG_WRAPPER"
    BINARY="$VG_WRAPPER"
    echo "Valgrind enabled — server runs under memcheck (logs: $VG_LOG_DIR)"
fi

# Scan valgrind logs; echoes a summary and returns non-zero if any error remains.
check_valgrind() {
    [ "$VALGRIND" = true ] || return 0
    local errs
    errs=$(grep -hc 'ERROR SUMMARY: [1-9]' "$VG_LOG_DIR"/vg.*.log 2>/dev/null | awk '{s+=$1} END{print s+0}')
    if [ "${errs:-0}" -gt 0 ]; then
        echo ""
        echo "VALGRIND ERRORS — memcheck reported issues in $errs server process(es):"
        grep -h -A12 'Invalid read\|Invalid write\|uninitialised\|Mismatched free\|Invalid free' "$VG_LOG_DIR"/vg.*.log 2>/dev/null | head -80
        return 1
    fi
    echo ""
    echo "Valgrind OK — no memcheck errors."
    return 0
}

# ── Fetch ScyllaDB tests ──────────────────────────────────────────────────
TESTS_DIR="$SCRIPT_DIR/scylla_cql_tests"
STAMP="$TESTS_DIR/.ref"

fetch_tests() {
    local ref="$1"
    local base_url="https://raw.githubusercontent.com/scylladb/scylladb/${ref}"

    echo "Fetching ScyllaDB CQL tests (ref: ${ref})..."
    rm -rf "$TESTS_DIR"

    # The upstream import structure is:
    #   validation/operations/insert_test.py  →  from ...porting import *
    #   cassandra_tests/porting.py            →  from ..util import unique_name
    #                                             from .. import nodetool
    # We mirror the hierarchy so relative imports resolve to our shims:
    #   .scylla_tests/              ← parent package ("test.cqlpy" equivalent)
    #     util.py                   ← our shim
    #     nodetool.py               ← our shim
    #     conftest.py               ← our pytest fixtures
    #     cassandra_tests/
    #       porting.py              ← UPSTREAM (unmodified)
    #       validation/operations/  ← UPSTREAM test files

    mkdir -p "$TESTS_DIR/cassandra_tests/validation/operations"
    mkdir -p "$TESTS_DIR/cassandra_tests/validation/entities"

    # Operation tests (from Apache Cassandra, ported to Python by ScyllaDB)
    local ops=(
        create_test.py
        insert_test.py
        select_test.py
        update_test.py
        delete_test.py
        drop_test.py
        truncate_test.py
        use_test.py
        alter_test.py
        batch_test.py
        select_limit_test.py
        select_order_by_test.py
    )
    for f in "${ops[@]}"; do
        curl -sfL "${base_url}/test/cqlpy/cassandra_tests/validation/operations/${f}" \
            -o "$TESTS_DIR/cassandra_tests/validation/operations/${f}" || true
    done

    # Entity tests
    local ents=(
        collections_test.py
        counters_test.py
        timestamp_test.py
        type_test.py
    )
    for f in "${ents[@]}"; do
        curl -sfL "${base_url}/test/cqlpy/cassandra_tests/validation/entities/${f}" \
            -o "$TESTS_DIR/cassandra_tests/validation/entities/${f}" || true
    done

    # Upstream porting layer (the test helper library)
    curl -sfL "${base_url}/test/cqlpy/cassandra_tests/porting.py" \
        -o "$TESTS_DIR/cassandra_tests/porting.py"

    # __init__.py files for package structure
    touch "$TESTS_DIR/__init__.py"
    touch "$TESTS_DIR/cassandra_tests/__init__.py"
    touch "$TESTS_DIR/cassandra_tests/validation/__init__.py"
    touch "$TESTS_DIR/cassandra_tests/validation/operations/__init__.py"
    touch "$TESTS_DIR/cassandra_tests/validation/entities/__init__.py"

    echo "$ref" > "$STAMP"
    echo "Tests fetched."
}

# Fetch if ref changed or not present
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$SCYLLA_REF" ]; then
    fetch_tests "$SCYLLA_REF"
else
    echo "Using cached ScyllaDB tests (ref: $SCYLLA_REF)"
fi

cp "$SCRIPT_DIR/conftest.py" "$TESTS_DIR/conftest.py"
cp "$SCRIPT_DIR/util.py"     "$TESTS_DIR/util.py"
cp "$SCRIPT_DIR/nodetool.py" "$TESTS_DIR/nodetool.py"
if [ -f "$SKIPLIST_FILE" ]; then
    cp "$SKIPLIST_FILE" "$TESTS_DIR/skiplist.txt"
fi
echo "Installed shims."

# ── Python venv ───────────────────────────────────────────────────────────
VENV="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV" ]; then
    echo "Creating venv with Python 3.12 via uv..."
    uv venv --python 3.12 "$VENV"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"
uv pip install -U pip setuptools wheel
uv pip install --no-build-isolation cassandra-driver
uv pip install pytest

# ── List mode ─────────────────────────────────────────────────────────────
# The parent of .scylla_tests/ must be on sys.path so that the package
# (whose __init__.py lives at .scylla_tests/__init__.py) is importable.
PARENT_OF_TESTS="$(dirname "$TESTS_DIR")"

if $LIST_ONLY; then
    echo ""
    python3 -m pytest "$TESTS_DIR/cassandra_tests/" \
        --collect-only -q \
        --override-ini="pythonpath=$PARENT_OF_TESTS" \
        --rootdir="$TESTS_DIR" \
        --confcutdir="$TESTS_DIR" \
        -c "$SCRIPT_DIR/pytest.ini" \
        ${PYTEST_K:+-k "$PYTEST_K"} \
        2>&1 || true
    exit 0
fi

# ── Run tests ─────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════════"
echo "  CQL Conformance Tests (scylladb ref: ${SCYLLA_REF})"
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
    python3 -m pytest "$TESTS_DIR/cassandra_tests/" \
        --host "$HOST" --port "$PORT" --binary "$BINARY" \
        "${PLUGIN_ARGS[@]}" \
        ${LOG_DIR:+--log-dir "$LOG_DIR"} \
        --override-ini="pythonpath=$PARENT_OF_TESTS" \
        --rootdir="$TESTS_DIR" \
        --confcutdir="$TESTS_DIR" \
        -c "$SCRIPT_DIR/pytest.ini" \
        -v --tb=short \
        ${PYTEST_K:+-k "$PYTEST_K"} \
        $PYTEST_EXTRA
}

if [ -n "$MUSTPASS_FILE" ] || [ -n "$OUTPUT_MUSTPASS" ]; then
    MUSTPASS_TMP="$(mktemp /tmp/plexdb_cql_pytest_XXXXXX.txt)"
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
        f.write('# Regenerate: tools/cql_tests/run.sh -o tools/cql_tests/mustpass.txt\n#\n')
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
    check_valgrind || MUSTPASS_EXIT=1
    exit $MUSTPASS_EXIT
else
    set +e
    run_pytest
    RC=$?
    set -e
    check_valgrind || RC=1
    exit $RC
fi
