# Minimal nodetool shim — cql does not support nodetool operations.
# These are no-ops so that porting.py functions like flush() don't crash.


def flush(cql, table):
    pass


def compact(cql, table):
    pass


def scylla_log(cql, msg, level="info"):
    pass
