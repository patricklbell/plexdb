# test_benchmark.py — runs redis-benchmark against resp_server if available.
# This file is copied into the test package by run.sh alongside conftest.py.

import shutil
import subprocess
import pytest


# Commands resp_server supports from redis-benchmark's built-in set
_SUPPORTED_TESTS = "ping_inline,ping_mbulk,set,get,mset"


def test_redis_benchmark_supported_commands(request, server_manager, r):
    """
    Run redis-benchmark for the commands resp_server actually implements.
    Skipped if redis-benchmark is not in PATH.
    """
    if not shutil.which("redis-benchmark"):
        pytest.skip("redis-benchmark not in PATH (install the redis package)")
    host = request.config.getoption("--host")
    port = request.config.getoption("--port")
    result = subprocess.run(
        [
            "redis-benchmark",
            "-h", host,
            "-p", str(port),
            "-n", "2000",
            "-q",
            "-t", _SUPPORTED_TESTS,
        ],
        capture_output=True,
        text=True,
    )
    print("\n" + result.stdout)
    if result.returncode != 0:
        print(result.stderr)
    assert result.returncode == 0, f"redis-benchmark exited with {result.returncode}"
