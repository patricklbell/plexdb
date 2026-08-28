# conftest.py — pytest fixtures and skip-helper shims for running redis-py's
# test_commands.py against server.
#
# This file serves two roles:
#  1. pytest conftest: provides fixtures (r, r2, pipe, slowlog, …) via
#     normal pytest injection.
#  2. Python module: importable as `tests.conftest` (because run.sh adds
#     redis_tests/ to sys.path) so that test_commands.py's module-level
#     `from tests.conftest import skip_if_server_version_lt` imports work.

import os
import shlex
import socket
import subprocess
import time

import pytest
import redis as redis_lib

# ── Version-check helpers (module-level, importable by test_commands.py) ──
# We advertise Redis 7.0.0 in INFO, so version guards behave accordingly.


def _ver(v: str):
    parts = v.split(".")
    return tuple(int(x) for x in parts[:3])


_SERVER_VERSION = "7.0.0"


def skip_if_server_version_lt(min_version):
    """Skip test if the server version is below min_version."""
    return pytest.mark.skipif(
        _ver(_SERVER_VERSION) < _ver(min_version),
        reason=f"Server {_SERVER_VERSION} < {min_version}",
    )


def skip_if_server_version_gte(max_version):
    """Skip test if the server version is >= max_version."""
    return pytest.mark.skipif(
        _ver(_SERVER_VERSION) >= _ver(max_version),
        reason=f"Server {_SERVER_VERSION} >= {max_version}",
    )


def skip_unless_arch_bits(arch_bits):
    """Skip test unless running on a specific word-size architecture."""
    import platform

    bits = 64 if platform.architecture()[0] == "64bit" else 32
    return pytest.mark.skipif(
        bits != arch_bits,
        reason=f"Requires {arch_bits}-bit architecture",
    )


def skip_if_redis_enterprise(func=None):
    """No-op: server is not Redis Enterprise.
    Supports both @skip_if_redis_enterprise and @skip_if_redis_enterprise().
    """
    if func is None:
        return lambda f: f
    return func


# ── CLI options ───────────────────────────────────────────────────────────


def pytest_addoption(parser):
    parser.addoption("--host", default="127.0.0.1", help="server host")
    parser.addoption("--port", default=6399, type=int, help="server port")
    parser.addoption(
        "--binary",
        default=None,
        help="Path to server binary; enables auto-start and auto-restart on crash",
    )
    parser.addoption(
        "--plugin",
        action="append",
        default=[],
        help="Path to a plugin .so to load via LD_PRELOAD (may be repeated)",
    )
    parser.addoption(
        "--db-path",
        default=None,
        help="Database file path for persistent mode; omit to run --in-memory",
    )
    parser.addoption(
        "--log-dir",
        default=None,
        help="Directory to write server stdout/stderr",
    )


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
    """Manages the server process lifecycle."""

    def __init__(self, binary, host, port, log_dir=None, plugins=None, db_path=None):
        self.binary = binary
        self.host = host
        self.port = port
        self.log_dir = log_dir
        self.plugins = plugins or []
        self.db_path = db_path  # None → --in-memory; str → persistent path
        self._proc = None
        self._tmp_db = None   # set when we own a temp file
        self._log_file = None

    def _spawn(self):
        self._tmp_db = None
        env = os.environ.copy()
        env.pop("LD_PRELOAD", None)
        if not self.plugins:
            env["PLEXDB_LOG_STDOUT"] = "1"
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        if self.db_path is not None:
            cmd = [self.binary, self.db_path, "--port", str(self.port), "--no-uring"]
        else:
            cmd = [self.binary, "--in-memory", "--port", str(self.port), "--no-uring"]
        if self.log_dir:
            os.makedirs(self.log_dir, exist_ok=True)
            log_path = os.path.join(self.log_dir, "server.log")
            self._log_file = open(log_path, "ab", buffering=0)
            stderr_target = self._log_file
            stdout_target = self._log_file
            if self.plugins:
                plugin_preload = ":".join(self.plugins)
                server_cmd = " ".join(shlex.quote(c) for c in cmd)
                shell_script = (
                    f'LD_PRELOAD="${{LD_PRELOAD:+$LD_PRELOAD:}}{plugin_preload}" '
                    f"exec {server_cmd}"
                )
                cmd = ["stdbuf", "-oL", "sh", "-c", shell_script]
            else:
                cmd = ["stdbuf", "-oL"] + cmd
            self._proc = subprocess.Popen(
                cmd,
                env=env,
                stdout=stdout_target,
                stderr=stderr_target,
            )
        else:
            stdout_target = None
            stderr_target = None
            if self.plugins:
                env["LD_PRELOAD"] = ":".join(self.plugins)
            self._proc = subprocess.Popen(
                cmd,
                env=env,
            )
        _wait_for_port(self.host, self.port)

    def start(self):
        if not os.path.isfile(self.binary):
            pytest.exit(
                f"error: server binary not found at '{self.binary}'\n",
                returncode=1,
            )
        self._spawn()
        print(f"server ready on {self.host}:{self.port}")

    def restart(self):
        if self._proc is not None:
            self._proc.kill()
            self._proc.wait()
            self._proc = None
        if self._tmp_db and os.path.exists(self._tmp_db):
            os.remove(self._tmp_db)
        self._spawn()

    def stop(self):
        if self._proc is not None:
            self._proc.kill()
            self._proc.wait()
            self._proc = None
        if self._tmp_db and os.path.exists(self._tmp_db):
            os.remove(self._tmp_db)
            self._tmp_db = None
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
    port = request.config.getoption("--port")
    log_dir = request.config.getoption("--log-dir")
    plugins = request.config.getoption("--plugin")
    db_path = request.config.getoption("--db-path")
    mgr = ServerManager(binary, host, port, log_dir=log_dir, plugins=plugins, db_path=db_path)
    mgr.start()
    yield mgr
    mgr.stop()


# ── Redis client fixtures ─────────────────────────────────────────────────


def _make_client(host, port, **kwargs):
    return redis_lib.Redis(host=host, port=port, **kwargs)


def _get_client(cls, request, *, single_connection_client=False, **kwargs):
    """Factory used by test_commands.py to create typed Redis clients."""
    host = request.config.getoption("--host")
    port = request.config.getoption("--port")
    return cls(host=host, port=port, **kwargs)


@pytest.fixture(scope="session")
def r(request, server_manager):
    """Primary Redis client (bytes mode, matching redis-py test expectations)."""
    host = request.config.getoption("--host")
    port = request.config.getoption("--port")
    client = _make_client(host, port, decode_responses=False)
    yield client
    client.close()


@pytest.fixture(scope="session")
def r2(request, server_manager):
    """Secondary Redis client (for tests that use two connections)."""
    host = request.config.getoption("--host")
    port = request.config.getoption("--port")
    client = _make_client(host, port, decode_responses=False)
    yield client
    client.close()


@pytest.fixture()
def pipe(r):
    """Pipeline fixture (non-transactional)."""
    return r.pipeline(False)


# ── Test isolation ────────────────────────────────────────────────────────


@pytest.fixture(autouse=True)
def resp_test_isolation(r, server_manager, request):
    """Flush DB before each test; detect crashes and restart server after."""
    # Pre-test: ensure connected and start clean
    try:
        r.flushall()
    except Exception:
        if server_manager is not None:
            server_manager.restart()
            r.connection_pool.disconnect()
            r.flushall()
        else:
            pytest.skip("Cannot connect to server")

    yield

    # Post-test: detect crash; restart so the next test can run
    try:
        r.ping()
    except Exception:
        if server_manager is not None:
            print(f"\nServer crashed during {request.node.nodeid}; restarting…")
            server_manager.restart()
            r.connection_pool.disconnect()
        else:
            pytest.exit(
                f"server crashed during {request.node.nodeid}",
                returncode=1,
            )


# ── Fixtures for features server does not implement ──────────────────


@pytest.fixture()
def slowlog(r):
    """SLOWLOG is not implemented — skip any test that requests this fixture."""
    pytest.skip("SLOWLOG not supported by server")
    yield []


@pytest.fixture()
def modifiable_slowlog_max_len(r):
    pytest.skip("SLOWLOG not supported by server")
    yield


@pytest.fixture()
def srand_seed(r):
    pytest.skip("DEBUG SLEEP/SRAND not supported by server")
    yield
