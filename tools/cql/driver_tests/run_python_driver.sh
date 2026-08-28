#!/usr/bin/env bash
# run_python_driver.sh — run DataStax python-driver's own integration test
# suite, UNMODIFIED, against a local plexdb_cql_server.
#
# This does not port or rewrite any driver tests. It clones the driver
# verbatim and runs its existing tests/integration/ suite by pointing it at
# an already-running server (the same USE_CASS_EXTERNAL mechanism the driver
# provides for testing against a hand-managed cluster instead of one CCM
# spins up itself). A handful of environment-level compatibility shims are
# needed, none of which touch driver test content:
#   - EVENT_LOOP_MANAGER=eventlet, since the driver's default connection-
#     class probing (gevent/eventlet/libev/asyncore) finds nothing usable on
#     a modern Python (asyncore was removed in 3.12) unless one is selected
#     and monkey-patched up front -- this is the driver's own documented way
#     of choosing a reactor, not a patch to it.
#   - build-libev-extension=false in the cloned driver's pyproject.toml,
#     because building the libev C extension needs libev-dev headers that
#     may not be installed; the driver works fine without it once a
#     different reactor is selected via EVENT_LOOP_MANAGER above.
#   - a two-line ccmlib/hcd_cluster.py stub, because the pip-published `ccm`
#     package predates HCD (Hyper-Converged Database) support that the
#     driver's test/integration/__init__.py imports unconditionally at
#     module load -- even though CCM itself is never actually invoked when
#     USE_CASS_EXTERNAL=1 skips real cluster management.
#
# Skiplisting works by -k substring deselection (pytest's own mechanism)
# instead of a conftest.py hook, since we don't touch the driver's conftest.
#
# Usage:
#   ./tools/cql/driver_tests/run_python_driver.sh [options]
#     -b <path>    plexdb_cql_server binary                       (default: ./build/cql/plexdb_cql_server)
#     -P <port>    native protocol port                           (default: 9042)
#     -r <ref>     python-driver git ref/tag                      (default: trunk)
#     -p <path>    test path within the driver repo, relative to
#                  tests/integration/                             (default: standard)
#     -t <expr>    pytest -k filter expression                    (default: run all)
#     -c <version> CASSANDRA_VERSION to report to the driver      (default: 3.11.19)
#     -m <file>    fail if a listed test regresses                (default: none)
#     -o <file>    write passing test IDs to <file> (gen mustpass) (default: none)
#     -s <file>    skiplist (deselect substrings)                 (default: tools/cql/driver_tests/skiplist.txt)
#     -h           show this help
set -euo pipefail

ORIG_DIR="$(pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

BINARY="${BINARY:-$ROOT_DIR/build/cql/plexdb_cql_server}"
PORT="9042"
DRIVER_REF="trunk"
TEST_PATH="standard"
PYTEST_K=""
CASS_VERSION="3.11.19"
MUSTPASS_FILE=""
OUTPUT_MUSTPASS=""
SKIPLIST_FILE="$SCRIPT_DIR/skiplist.txt"

while getopts "b:P:r:p:t:c:m:o:s:h" opt; do
    case "$opt" in
        b) BINARY="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        r) DRIVER_REF="$OPTARG" ;;
        p) TEST_PATH="$OPTARG" ;;
        t) PYTEST_K="$OPTARG" ;;
        c) CASS_VERSION="$OPTARG" ;;
        m) MUSTPASS_FILE="$OPTARG" ;;
        o) OUTPUT_MUSTPASS="$OPTARG" ;;
        s) SKIPLIST_FILE="$OPTARG" ;;
        h)
            sed -n '2,37p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

# Resolve before the later `cd "$DRIVER_DIR"` so relative paths are relative
# to the caller's invocation directory, not the driver checkout.
[ -n "$MUSTPASS_FILE" ] && MUSTPASS_FILE="$(cd "$(dirname "$MUSTPASS_FILE")" && pwd)/$(basename "$MUSTPASS_FILE")"
if [ -n "$OUTPUT_MUSTPASS" ]; then
    mkdir -p "$(dirname "$ORIG_DIR/$OUTPUT_MUSTPASS")"
    OUTPUT_MUSTPASS="$ORIG_DIR/$OUTPUT_MUSTPASS"
fi
[ -f "$SKIPLIST_FILE" ] || SKIPLIST_FILE="$ORIG_DIR/$SKIPLIST_FILE"

DRIVER_DIR="$SCRIPT_DIR/python-driver"
STAMP="$DRIVER_DIR/.ref"

# ── Fetch python-driver (verbatim) ────────────────────────────────────────
if [ ! -d "$DRIVER_DIR" ] || [ "$(cat "$STAMP" 2>/dev/null || true)" != "$DRIVER_REF" ]; then
    rm -rf "$DRIVER_DIR"
    git clone --depth 1 --branch "$DRIVER_REF" https://github.com/datastax/python-driver.git "$DRIVER_DIR"
    # Environment-level shim (see file header): we can't always build the
    # optional libev C extension, and don't need it once EVENT_LOOP_MANAGER
    # selects a different reactor.
    sed -i 's/build-libev-extension = true/build-libev-extension = false/' "$DRIVER_DIR/pyproject.toml"
    echo "$DRIVER_REF" > "$STAMP"
else
    echo "Using cached python-driver (ref: $DRIVER_REF)"
fi

# ── Python venv ───────────────────────────────────────────────────────────
VENV="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV" ]; then
    uv venv --python 3.12 "$VENV"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"
uv pip install -q -U pip setuptools wheel Cython
uv pip install -q --no-build-isolation -e "$DRIVER_DIR"
uv pip install -q pytest "ccm==3.1.5" pytz pure-sasl six gevent eventlet scales cryptography

# Environment-level shim (see file header): pip's ccm predates HCD support,
# but the driver's tests/integration/__init__.py imports it unconditionally.
CCMLIB_DIR="$(python3 -c 'import ccmlib, os; print(os.path.dirname(ccmlib.__file__))')"
if [ ! -f "$CCMLIB_DIR/hcd_cluster.py" ]; then
    cat > "$CCMLIB_DIR/hcd_cluster.py" <<'EOF'
# Local stub: this ccm release predates HCD support. Never actually
# instantiated since USE_CASS_EXTERNAL=1 bypasses real CCM cluster management.
from ccmlib.cluster import Cluster as HcdCluster
EOF
fi

# ── Build -k expression (user filter AND skiplist deselection) ───────────
K_EXPR="$PYTEST_K"
if [ -f "$SKIPLIST_FILE" ]; then
    while IFS= read -r raw; do
        line="${raw%%#*}"
        line="$(echo "$line" | xargs || true)"
        [ -z "$line" ] && continue
        if [ -z "$K_EXPR" ]; then
            K_EXPR="not $line"
        else
            K_EXPR="($K_EXPR) and not $line"
        fi
    done < "$SKIPLIST_FILE"
fi

# ── Start server ──────────────────────────────────────────────────────────
DB_DIR="$(mktemp -d)"
"$BINARY" "$DB_DIR/test.db" --port "$PORT" --no-uring > "$DB_DIR/server.log" 2>&1 &
SERVER_PID=$!
cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$DB_DIR"
}
trap cleanup EXIT
sleep 1

# ── Run the driver's own integration suite, unmodified ───────────────────
export USE_CASS_EXTERNAL=1
export CASSANDRA_VERSION="$CASS_VERSION"
export EVENT_LOOP_MANAGER=eventlet
export CLUSTER_IP=127.0.0.1

cd "$DRIVER_DIR"

run_pytest() {
    python3 -m pytest "tests/integration/${TEST_PATH}" -v --tb=short --no-header -p no:warnings \
        --continue-on-collection-errors \
        ${K_EXPR:+-k "$K_EXPR"}
}

if [ -n "$MUSTPASS_FILE" ] || [ -n "$OUTPUT_MUSTPASS" ]; then
    MUSTPASS_TMP="$(mktemp /tmp/plexdb_pydriver_pytest_XXXXXX.txt)"
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
        f.write('# Mustpass list -- tests that must pass to prevent regressions.\n')
        f.write('# Generated: ' + datetime.now().strftime('%Y-%m-%d') + '\n')
        f.write('# Regenerate: tools/cql/driver_tests/run_python_driver.sh -o tools/cql/driver_tests/mustpass.txt\n#\n')
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
        print(f'\nMUSTPASS REGRESSIONS -- {len(regressions)} test(s) that must pass have failed:')
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
