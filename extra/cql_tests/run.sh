#!/usr/bin/env bash
# run.sh — fetch ScyllaDB CQL tests and run them against a local objstore instance.
#
# The tests come from scylladb/scylladb's test/cqlpy/cassandra_tests/ directory,
# which contains Apache Cassandra CQL tests ported to Python+pytest. Only a
# compatibility shim (conftest.py, porting.py, util.py) is local; the actual
# test content is fetched verbatim from the upstream repository.
#
# Usage:
#   ./extra/cql_tests/run.sh [options]
#     -b <path>    objstore_server binary  (default: ./build/objstore/objstore_server)
#     -P <port>    native protocol port    (default: 9042)
#     -r <ref>     scylladb git ref/tag    (default: master)
#     -t <expr>    pytest -k expression    (default: run all)
#     -x           stop on first failure   (pytest -x)
#     -l           list tests only         (pytest --collect-only)
#     -L <dir>     write per-session server log to <dir>/server.log
#     -h           show this help
#
# Environment:
#   CQL_TEST_HOST    override contact point (default: 127.0.0.1)
#   CQL_TEST_PORT    override port          (default: value of -P)
#   SCYLLA_REF       same as -r

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY="${BINARY:-$ROOT_DIR/build/objstore/objstore_server}"
PORT="${PORT:-9042}"
SCYLLA_REF="${SCYLLA_REF:-master}"
PYTEST_K=""
PYTEST_EXTRA=""
LIST_ONLY=false
LOG_DIR=""

while getopts "b:P:r:t:xlL:h" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        r) SCYLLA_REF="$OPTARG" ;;
        t) PYTEST_K="$OPTARG" ;;
        x) PYTEST_EXTRA="$PYTEST_EXTRA -x" ;;
        l) LIST_ONLY=true ;;
        L) LOG_DIR="$OPTARG" ;;
        h)
            sed -n '2,24p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

HOST="${CQL_TEST_HOST:-127.0.0.1}"
PORT="${CQL_TEST_PORT:-$PORT}"

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

python3 -m pytest "$TESTS_DIR/cassandra_tests/" \
    --host "$HOST" --port "$PORT" --binary "$BINARY" \
    ${LOG_DIR:+--log-dir "$LOG_DIR"} \
    --override-ini="pythonpath=$PARENT_OF_TESTS" \
    --rootdir="$TESTS_DIR" \
    --confcutdir="$TESTS_DIR" \
    -c "$SCRIPT_DIR/pytest.ini" \
    -v --tb=short \
    ${PYTEST_K:+-k "$PYTEST_K"} \
    $PYTEST_EXTRA
