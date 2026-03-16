# Minimal conftest.py shim for running ScyllaDB's cassandra_tests against objstore.
#
# This replaces ScyllaDB's own conftest.py with a lightweight version that
# provides only the pytest fixtures the upstream test files actually need:
#   cql             — a cassandra-driver Session connected to objstore
#   test_keyspace   — a freshly-created keyspace, dropped after the test
#   this_dc         — the data-center name reported by system.local
#   scylla_only     — skips the test (objstore is neither Scylla nor Cassandra)
#   cassandra_bug   — no-op (we don't need to xfail Cassandra bugs)

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

# ── Fixtures ──────────────────────────────────────────────────────────────

@pytest.fixture(scope="session")
def cql(request):
    host = request.config.getoption("--host")
    port = int(request.config.getoption("--port"))
    profile = ExecutionProfile(
        load_balancing_policy=RoundRobinPolicy(),
        consistency_level=ConsistencyLevel.ONE,
        request_timeout=30,
    )
    cluster = Cluster(
        contact_points=[host],
        port=port,
        protocol_version=4,
        execution_profiles={EXEC_PROFILE_DEFAULT: profile},
        connect_timeout=30,
        control_connection_timeout=30,
    )
    try:
        session = cluster.connect()
    except NoHostAvailable:
        pytest.exit(f"Cannot connect to objstore at {host}:{port}",
                    returncode=pytest.ExitCode.INTERNAL_ERROR)
    yield session
    cluster.shutdown()

# After every test function, check the connection is still alive.
@pytest.fixture(scope="function", autouse=True)
def cql_test_connection(cql, request):
    if cql_test_connection.server_crashed:
        pytest.skip("Server down")
    yield
    try:
        cql.execute("SELECT * FROM system.local")
    except Exception:
        cql_test_connection.server_crashed = True
        pytest.exit(
            f"objstore appears to have crashed during {request.node.parent.name}::{request.node.name}",
            returncode=pytest.ExitCode.INTERNAL_ERROR,
        )

cql_test_connection.server_crashed = False

@pytest.fixture(scope="session")
def this_dc(cql):
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
