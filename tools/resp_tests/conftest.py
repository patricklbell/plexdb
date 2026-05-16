# conftest.py — pytest fixtures and skip-helper shims for running redis-py's
# test_commands.py against resp_server.
#
# This file serves two roles:
#  1. pytest conftest: provides fixtures (r, r2, pipe, slowlog, …) via
#     normal pytest injection.
#  2. Python module: importable as `tests.conftest` (because run.sh adds
#     redis_tests/ to sys.path) so that test_commands.py's module-level
#     `from tests.conftest import skip_if_server_version_lt` imports work.

import os
import shutil
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
    """No-op: resp_server is not Redis Enterprise.
    Supports both @skip_if_redis_enterprise and @skip_if_redis_enterprise().
    """
    if func is None:
        return lambda f: f
    return func


# ── CLI options ───────────────────────────────────────────────────────────

def pytest_addoption(parser):
    parser.addoption(
        "--host", default="127.0.0.1", help="resp_server host"
    )
    parser.addoption(
        "--port", default=6399, type=int, help="resp_server port"
    )
    parser.addoption(
        "--binary",
        default=None,
        help="Path to resp_server binary; enables auto-start and auto-restart on crash",
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
    """Manages the resp_server process lifecycle."""

    def __init__(self, binary, host, port, log_dir=None):
        self.binary = binary
        self.host = host
        self.port = port
        self.log_dir = log_dir
        self._proc = None
        self._log_file = None

    def _spawn(self):
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        # --no-uring: use async sockets (more portable, avoids io_uring quirks)
        cmd = [self.binary, "--port", str(self.port), "--no-uring"]
        if self.log_dir:
            os.makedirs(self.log_dir, exist_ok=True)
            log_path = os.path.join(self.log_dir, "server.log")
            self._log_file = open(log_path, "ab", buffering=0)
            stdbuf = shutil.which("stdbuf")
            if stdbuf:
                cmd = [stdbuf, "-oL"] + cmd
            self._proc = subprocess.Popen(
                cmd,
                stdout=self._log_file,
                stderr=self._log_file,
            )
        else:
            self._proc = subprocess.Popen(cmd)
        _wait_for_port(self.host, self.port)

    def start(self):
        if not os.path.isfile(self.binary):
            pytest.exit(
                f"error: resp_server binary not found at '{self.binary}'\n"
                f"Build with:  cmake -B build -G Ninja && ninja -C build resp_server",
                returncode=1,
            )
        self._spawn()
        print(f"resp_server ready on {self.host}:{self.port}")

    def restart(self):
        if self._proc is not None:
            self._proc.kill()
            self._proc.wait()
            self._proc = None
        self._spawn()

    def stop(self):
        if self._proc is not None:
            self._proc.kill()
            self._proc.wait()
            self._proc = None
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
    mgr = ServerManager(binary, host, port, log_dir=log_dir)
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
            pytest.skip("Cannot connect to resp_server")

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
                f"resp_server crashed during {request.node.nodeid}",
                returncode=1,
            )


# ── Fixtures for features resp_server does not implement ──────────────────

@pytest.fixture()
def slowlog(r):
    """SLOWLOG is not implemented — skip any test that requests this fixture."""
    pytest.skip("SLOWLOG not supported by resp_server")
    yield []


@pytest.fixture()
def modifiable_slowlog_max_len(r):
    pytest.skip("SLOWLOG not supported by resp_server")
    yield


@pytest.fixture()
def srand_seed(r):
    pytest.skip("DEBUG SLEEP/SRAND not supported by resp_server")
    yield
