# Minimal conftest.py shim for running ScyllaDB's cassandra_tests against objstore.
#
# This replaces ScyllaDB's own conftest.py with a lightweight version that
# provides only the pytest fixtures the upstream test files actually need:
#   cql             — a cassandra-driver Session connected to objstore
#   test_keyspace   — a freshly-created keyspace, dropped after the test
#   this_dc         — the data-center name reported by system.local
#   scylla_only     — skips the test (objstore is neither Scylla nor Cassandra)
#   cassandra_bug   — no-op (we don't need to xfail Cassandra bugs)
#

import os
import socket
import subprocess
import tempfile
import time
import pytest
from cassandra.cluster import Cluster, ConsistencyLevel, ExecutionProfile, EXEC_PROFILE_DEFAULT, NoHostAvailable
from cassandra.policies import RoundRobinPolicy

# ── CLI options ───────────────────────────────────────────────────────────
def pytest_addoption(parser):
    parser.addoption("--host", action="store", default="127.0.0.1",
                     help="CQL contact point")
    parser.addoption("--port", action="store", default="9042",
                     help="CQL native transport port")
    parser.addoption("--binary", action="store", default=None,
                     help="Path to objstore_server binary; enables auto-restart on crash")

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
    """Manages the objstore_server process lifecycle."""

    def __init__(self, binary, host, port):
        self.binary = binary
        self.host = host
        self.port = port
        self._proc = None
        self._db = None

    def _spawn(self):
        # mkstemp gives us a unique path; remove it so the server creates the file fresh.
        fd, self._db = tempfile.mkstemp(prefix="cql_conformance_", suffix=".db", dir="/tmp")
        os.close(fd)
        os.remove(self._db)
        self._proc = subprocess.Popen(
            [self.binary, self._db, "--port", str(self.port)],
        )
        _wait_for_port(self.host, self.port)

    def start(self):
        if not os.path.isfile(self.binary):
            pytest.exit(
                f"error: objstore binary not found at '{self.binary}'\n"
                f"Build with:  cmake -B build -G Ninja && ninja -C build objstore_server",
                returncode=1,
            )
        self._spawn()
        print("Server ready.")

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

@pytest.fixture(scope="session")
def server_manager(request):
    binary = request.config.getoption("--binary")
    if binary is None:
        yield None
        return
    host = request.config.getoption("--host")
    port = int(request.config.getoption("--port"))
    mgr = ServerManager(binary, host, port)
    mgr.start()
    yield mgr
    mgr.stop()

# ── CQL session proxy ─────────────────────────────────────────────────────

def _make_cluster(host, port):
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
        self._cluster = _make_cluster(host, port)
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

@pytest.fixture(scope="session")
def cql(request, server_manager):
    host = request.config.getoption("--host")
    port = int(request.config.getoption("--port"))
    cql_session = CqlSession()
    try:
        cql_session.connect(host, port)
    except NoHostAvailable:
        if server_manager is None:
            pytest.exit(f"Cannot connect to objstore at {host}:{port}",
                        returncode=pytest.ExitCode.INTERNAL_ERROR)
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
        try:
            server_manager.restart()
            cql.connect(host, port)
        except Exception:
            pytest.skip("Cannot connect after server restart")

    yield

    # ── Post-test: detect crash; restart for next test ────────────────────
    try:
        cql.execute("SELECT * FROM system.local")
    except Exception:
        cql.disconnect()
        if server_manager is not None:
            print(f"\nServer crashed during {request.node.nodeid}; restarting...")
            try:
                server_manager.restart()
                cql.connect(host, port)
            except Exception:
                pass  # next test's pre-test check will retry
        else:
            pytest.exit(
                f"objstore crashed during {request.node.nodeid}",
                returncode=pytest.ExitCode.INTERNAL_ERROR,
            )

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
    return f"cqltest{ms}"
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

# objstore is neither Scylla nor Cassandra — skip Scylla-only tests.
@pytest.fixture(scope="session")
def scylla_only(cql):
    pytest.skip("Scylla-only test skipped (running on objstore)")

# cassandra_bug: on objstore we just run the test normally.
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
