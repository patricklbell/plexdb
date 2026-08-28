# Minimal shim for ScyllaDB's test.pylib.skip_types module — provides just the
# runtime skip helper(s) that upstream cassandra_tests files import. Upstream's
# version wires typed skip categories into a pytest plugin (skip_reason_plugin)
# for JUnit reporting; this shim only needs to actually perform the skip.

import pytest


def skip_env(reason):
    pytest.skip(f"[env] {reason}")
