# Minimal conftest.py shim for running ScyllaDB's cassandra_tests against cql.
#
# This replaces ScyllaDB's own conftest.py with a lightweight version that
# provides only the pytest fixtures the upstream test files actually need:
#   cql             — a cassandra-driver Session connected to cql
#   test_keyspace   — a freshly-created keyspace, dropped after the test
#   this_dc         — the data-center name reported by system.local
#   scylla_only     — skips the test (cql is neither Scylla nor Cassandra)
#   cassandra_bug   — no-op (we don't need to xfail Cassandra bugs)
#

import os
import shlex
import socket
import subprocess
import sys
import tempfile
import time

import pytest

# run.sh copies tools/conformance_common/server_crash.py next to this file.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server_crash  # noqa: E402
from cassandra.cluster import (
    EXEC_PROFILE_DEFAULT,
    Cluster,
    ConsistencyLevel,
    ExecutionProfile,
    NoHostAvailable,
)
from cassandra.policies import RoundRobinPolicy


# ── Skiplist (permissive-parser deselects) ────────────────────────────────
# Skiplist entries name upstream tests that exercise parsing shapes plexdb
# intentionally accepts (more permissive than Cassandra). See skiplist.txt
# for the file format. Tests that exercise genuinely fatal (unparsable /
# ambiguous) parsing must NOT be listed and continue to run.


def _load_skiplist(path):
    entries: list[tuple[str, str]] = []
    try:
        with open(path) as f:
            for raw in f:
                line = raw.rstrip("\n")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                substr, _, reason = line.partition("#")
                entries.append((substr.strip(), reason.strip() or "skiplist"))
    except FileNotFoundError:
        pass
    return entries


# @note skip_bug is deliberately excluded — it marks tests ScyllaDB's own
# implementation skips as flaky/buggy, which says nothing about whether this
# engine (a different, synchronous implementation) reproduces that flakiness.
# Honoring it blindly skipped testNowToUUIDCompatibility/testReversedTypeCompatibility
# even though both pass reliably here.
#
# skip_env IS honored — it marks tests that need something the test harness
# itself doesn't run (e.g. a vector store service), which is missing here too
# regardless of engine.
_UPSTREAM_SKIP_MARKERS = ("skip_not_implemented", "skip_env")


def pytest_collection_modifyitems(config, items):
    skiplist_path = config.getoption("--skiplist")
    skip_marks = [
        (substr, pytest.mark.skip(reason=reason))
        for substr, reason in _load_skiplist(skiplist_path)
    ]
    for item in items:
        for substr, mark in skip_marks:
            if substr in item.nodeid:
                item.add_marker(mark)
                break
        for marker_name in _UPSTREAM_SKIP_MARKERS:
            marker = item.get_closest_marker(marker_name)
            if marker is not None:
                reason = marker.kwargs.get("reason", marker_name)
                item.add_marker(pytest.mark.skip(reason=reason))


# ── CLI options ───────────────────────────────────────────────────────────
def pytest_addoption(parser):
    parser.addoption(
        "--skiplist",
        action="store",
        default=os.path.join(os.path.dirname(__file__), "skiplist.txt"),
        help="Path to skiplist file (test nodeid substrings to deselect)",
    )
    parser.addoption(
        "--host", action="store", default="127.0.0.1", help="CQL contact point"
    )
    parser.addoption(
        "--port", action="store", default="9042", help="CQL native transport port"
    )
    parser.addoption(
        "--binary",
        action="store",
        default=None,
        help="Path to plexdb_cql_server binary; enables auto-restart on crash",
    )
    parser.addoption(
        "--plugin",
        action="append",
        default=[],
        help="Path to a plugin .so to load via LD_PRELOAD (may be repeated)",
    )
    parser.addoption(
        "--log-dir",
        action="store",
        default=None,
        help="Directory to write per-test server logs",
    )


# ── Server crash attribution ──────────────────────────────────────────────


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    server_crash.on_setup(item)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    server_crash.on_report(item, outcome.get_result())


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    server_crash.terminal_summary(terminalreporter, config)


# ── Server manager ────────────────────────────────────────────────────────


def _wait_for_port(host, port, attempts=50, delay=0.2):
    for _ in range(attempts):
        try:
            s = socket.create_connection((host, port), timeout=1)
            s.close()
            return
        except OSError:
            time.sleep(delay)
    raise RuntimeError(f"Server did not become ready on {host}:{port}")


class ServerManager:
    """Manages the plexdb_cql_server process lifecycle."""

    def __init__(self, binary, host, port, log_dir=None, plugins=None):
        self.binary = binary
        self.host = host
        self.port = port
        self.log = server_crash.ServerLog(log_dir)
        self.plugins = plugins or []
        self._proc = None
        self._db = None
        self._log_file = None

    def _spawn(self):
        # mkstemp gives us a unique path; remove it so the server creates the file fresh.
        fd, self._db = tempfile.mkstemp(
            prefix="cql_conformance_", suffix=".db", dir="/tmp"
        )
        os.close(fd)
        os.remove(self._db)
        env = os.environ.copy()
        env.pop("LD_PRELOAD", None)
        if not self.plugins:
            env["PLEXDB_LOG_STDOUT"] = "1"
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        cmd = [self.binary, self._db, "--port", str(self.port)]
        self._log_file = self.log.open()
        if self.plugins:
            # Run stdbuf without LD_PRELOAD so it doesn't try to load the plugin
            # (plugin symbols don't exist in stdbuf). stdbuf will exec sh with
            # LD_PRELOAD=libstdbuf.so; sh then appends our plugins and execs the
            # server, which ends up with LD_PRELOAD=libstdbuf.so:plugin.so.
            plugin_preload = ":".join(self.plugins)
            server_cmd = " ".join(shlex.quote(c) for c in cmd)
            shell_script = (
                f'LD_PRELOAD="${{LD_PRELOAD:+$LD_PRELOAD:}}{plugin_preload}" '
                f"exec {server_cmd}"
            )
            cmd = ["stdbuf", "-oL", "sh", "-c", shell_script]
        else:
            # Force line-buffered stdout so logs appear before server is killed
            cmd = ["stdbuf", "-oL"] + cmd
        self._proc = subprocess.Popen(
            cmd,
            env=env,
            stderr=self._log_file,
            stdout=self._log_file,
        )
        _wait_for_port(self.host, self.port)

    def start(self):
        if not os.path.isfile(self.binary):
            pytest.exit(
                f"error: cql binary not found at '{self.binary}'\n"
                f"Build with:  cmake -B build -G Ninja && ninja -C build plexdb_cql_server",
                returncode=1,
            )
        self._spawn()
        print("Server ready.")

    def exited(self, timeout):
        """Return the server's exit status if it has died (waiting up to timeout), else None."""
        if self._proc is None:
            return None
        try:
            return self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def restart(self):
        if self._proc is not None:
            self._proc.kill()
            self._proc.wait()
        if self._db and os.path.exists(self._db):
            os.remove(self._db)
        self._spawn()

    def stop(self):
        if self._proc is not None:
            self._proc.kill()
            self._proc.wait()
            self._proc = None
        if self._db and os.path.exists(self._db):
            os.remove(self._db)
            self._db = None
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None


@pytest.fixture(scope="session")
def server_manager(request):
    binary = request.config.getoption("--binary")
    if binary is None:
        yield None
        return
    host = request.config.getoption("--host")
    port = int(request.config.getoption("--port"))
    log_dir = request.config.getoption("--log-dir")
    plugins = request.config.getoption("--plugin")
    mgr = ServerManager(binary, host, port, log_dir=log_dir, plugins=plugins)
    mgr.start()
    server_crash.register(request.config, mgr)
    yield mgr
    mgr.stop()


# ── CQL session proxy ─────────────────────────────────────────────────────


def _create_cluster(host, port):
    profile = ExecutionProfile(
        load_balancing_policy=RoundRobinPolicy(),
        consistency_level=ConsistencyLevel.ONE,
        request_timeout=30,
    )
    return Cluster(
        contact_points=[host],
        port=port,
        protocol_version=4,
        execution_profiles={EXEC_PROFILE_DEFAULT: profile},
        connect_timeout=30,
        control_connection_timeout=30,
    )


class CqlSession:
    """Proxy around a cassandra Session; supports disconnect/reconnect between tests."""

    def __init__(self):
        self._cluster = None
        self._session = None

    @property
    def connected(self):
        return self._session is not None

    def connect(self, host, port):
        """Open a new connection, closing any existing one. Raises NoHostAvailable on failure."""
        if self._cluster is not None:
            try:
                self._cluster.shutdown()
            except Exception:
                pass
            self._cluster = None
            self._session = None
        self._cluster = _create_cluster(host, port)
        self._session = self._cluster.connect()  # raises NoHostAvailable on failure

    def disconnect(self):
        self._session = None

    def __getattr__(self, name):
        return getattr(self._session, name)

    def shutdown(self):
        if self._cluster is not None:
            try:
                self._cluster.shutdown()
            except Exception:
                pass


# ── Fixtures ──────────────────────────────────────────────────────────────


def _connect_active(cql_session, host, port, attempts=60, delay=0.5):
    """Wait for the server to be CQL-active, not merely listening.

    _wait_for_port only confirms the TCP port is open; the CQL handshake may not
    be ready yet (notably under slower builds like ASAN), which otherwise makes
    every test skip. Retry the driver connect until it succeeds or we give up.
    Returns True on success.
    """
    for _ in range(attempts):
        try:
            cql_session.connect(host, port)
            return True
        except NoHostAvailable:
            time.sleep(delay)
    return False


@pytest.fixture(scope="session")
def cql(request, server_manager):
    host = request.config.getoption("--host")
    port = int(request.config.getoption("--port"))
    cql_session = CqlSession()
    if not _connect_active(cql_session, host, port):
        if server_manager is None:
            pytest.exit(
                f"Cannot connect to cql at {host}:{port}",
                returncode=pytest.ExitCode.INTERNAL_ERROR,
            )
        # else: cql_test_connection will restart+reconnect before each test
    yield cql_session
    cql_session.shutdown()


# Autouse fixture that runs around every test:
#   setup   — ensure a live CQL connection; restart server + reconnect if needed
#   teardown — detect a crash; restart + reconnect so the next test can try
@pytest.fixture(scope="function", autouse=True)
def cql_test_connection(cql, server_manager, request):
    host = request.config.getoption("--host")
    port = int(request.config.getoption("--port"))

    # ── Pre-test: ensure connected ────────────────────────────────────────
    if not cql.connected:
        if server_manager is None:
            pytest.skip("No connection to server")
        server_manager.restart()
        if not _connect_active(cql, host, port):
            pytest.skip("Cannot connect after server restart")

    yield

    # ── Post-test: detect crash; restart for next test ────────────────────
    try:
        cql.execute("SELECT * FROM system.local")
    except Exception:
        cql.disconnect()
        if server_manager is not None:
            print(f"\nServer crashed during {request.node.nodeid}; restarting...")
            server_manager.restart()
            _connect_active(cql, host, port)  # best-effort; next test's pre-test check retries
        else:
            pytest.exit(
                f"cql crashed during {request.node.nodeid}",
                returncode=pytest.ExitCode.INTERNAL_ERROR,
            )


# Keyspaces owned by the server that must never be dropped between tests.
_SYSTEM_KEYSPACES = frozenset(
    {
        "system",
        "system_schema",
        "system_auth",
        "system_distributed",
        "system_traces",
        "system_views",
        "system_virtual_schema",
    }
)


def _drop_user_keyspaces(cql):
    """Drop every non-system keyspace (cascading to its tables/types) so the schema
    is clean. Best-effort: ignores failures so a wedged server doesn't abort the run."""
    if not cql.connected:
        return
    try:
        rows = list(cql.execute("SELECT keyspace_name FROM system_schema.keyspaces"))
    except Exception:
        return
    for row in rows:
        ks = row[0]
        if ks in _SYSTEM_KEYSPACES:
            continue
        try:
            cql.execute(f"DROP KEYSPACE {ks}")
        except Exception:
            pass


# Autouse: enforce test independence. The suite shares one session-scoped server, so a
# keyspace leaked by a prior test — or one whose context-manager cleanup was skipped after
# a failure/crash — would otherwise bleed into the next test (order-dependent flakiness and
# unbounded schema growth). Reset to a system-only schema before every test.
@pytest.fixture(scope="function", autouse=True)
def reset_shared_state(cql_test_connection, cql):
    _drop_user_keyspaces(cql)
    yield


@pytest.fixture(scope="session")
def this_dc(cql):
    if not cql.connected:
        yield "datacenter1"
        return
    row = cql.execute("SELECT data_center FROM system.local").one()
    yield row[0] if row else "datacenter1"


def _unique_ks():
    ms = int(round(time.time() * 1000))
    if _unique_ks.last >= ms:
        ms = _unique_ks.last + 1
    _unique_ks.last = ms
    # Distinct prefix from the tests' own unique_name() ("cqltest<ms>"): both are millisecond
    # based with independent counters, so a shared prefix collides when a test uses this fixture
    # and create_keyspace()/create_table() in the same millisecond → spurious AlreadyExists.
    return f"ksfixture{ms}"


_unique_ks.last = 0


@pytest.fixture(scope="function")
def test_keyspace(cql, this_dc):
    ks = _unique_ks()
    cql.execute(
        f"CREATE KEYSPACE {ks} WITH replication = "
        f"{{'class': 'SimpleStrategy', 'replication_factor': 1}}"
    )
    yield ks
    try:
        cql.execute(f"DROP KEYSPACE {ks}")
    except Exception:
        pass


# cql is neither Scylla nor Cassandra — skip Scylla-only tests.
@pytest.fixture(scope="session")
def scylla_only(cql):
    pytest.skip("Scylla-only test skipped (running on cql)")


# cassandra_bug: on cql we just run the test normally.
@pytest.fixture(scope="session")
def cassandra_bug():
    pass


@pytest.fixture(scope="session")
def has_tablets():
    return False


@pytest.fixture(scope="function")
def skip_without_tablets():
    pytest.skip("Tablets not supported")


@pytest.fixture(scope="function")
def driver_bug_1():
    pass


@pytest.fixture(scope="function")
def random_seed():
    import random

    seed = time.time()
    random.seed(seed)
    yield seed


@pytest.fixture(scope="function")
def compact_storage():
    pytest.skip("Compact storage not supported")
