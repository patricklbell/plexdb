# Minimal util.py shim — provides helper functions expected by ScyllaDB's
# cassandra_tests porting layer and test files.

import time
import string
import random
import collections
from contextlib import contextmanager

def random_string(length=10, chars=string.ascii_uppercase + string.digits):
    return ''.join(random.choice(chars) for x in range(length))

def random_bytes(length=10):
    return bytearray(random.getrandbits(8) for _ in range(length))

# Unique names that are valid unquoted CQL identifiers.
unique_name_prefix = 'cqltest'
def unique_name():
    current_ms = int(round(time.time() * 1000))
    if unique_name.last_ms >= current_ms:
        current_ms = unique_name.last_ms + 1
    unique_name.last_ms = current_ms
    return unique_name_prefix + str(current_ms)
unique_name.last_ms = 0

def unique_key_string():
    unique_key_string.i += 1
    return 's' + str(unique_key_string.i)
unique_key_string.i = 0

def unique_key_int():
    unique_key_int.i += 1
    return unique_key_int.i
unique_key_int.i = 0

def is_scylla(cql):
    return False

def keyspace_has_tablets(cql, keyspace):
    return False

@contextmanager
def new_test_keyspace(cql, _opts):
    # @note keep _opts for upstream helper compatibility; objstore currently creates keyspaces without options.
    keyspace = unique_name()
    cql.execute("CREATE KEYSPACE " + keyspace)
    try:
        yield keyspace
    finally:
        cql.execute("DROP KEYSPACE " + keyspace)

previously_used_table_names = []
@contextmanager
def new_test_table(cql, keyspace, schema, extra=""):
    global previously_used_table_names
    if not previously_used_table_names:
        previously_used_table_names.append(unique_name())
    table_name = previously_used_table_names.pop()
    table = keyspace + "." + table_name
    cql.execute("CREATE TABLE " + table + "(" + schema + ")" + extra)
    try:
        yield table
    finally:
        cql.execute("DROP TABLE " + table)
        previously_used_table_names.append(table_name)

@contextmanager
def new_type(cql, keyspace, cmd, name=None):
    type_name = keyspace + "." + (name or unique_name())
    cql.execute("CREATE TYPE " + type_name + " " + cmd)
    try:
        yield type_name
    finally:
        cql.execute("DROP TYPE " + type_name)

@contextmanager
def new_function(cql, keyspace, body, name=None, args=None):
    fun = name if name else unique_name()
    cql.execute(f"CREATE FUNCTION {keyspace}.{fun} {body}")
    try:
        yield fun
    finally:
        if args:
            cql.execute(f"DROP FUNCTION {keyspace}.{fun}({args})")
        else:
            cql.execute(f"DROP FUNCTION {keyspace}.{fun}")

@contextmanager
def new_materialized_view(cql, table, select, pk, where, extra=""):
    keyspace = table.split('.')[0]
    mv = keyspace + "." + unique_name()
    cql.execute(f"CREATE MATERIALIZED VIEW {mv} AS SELECT {select} FROM {table} WHERE {where} PRIMARY KEY ({pk}) {extra}")
    try:
        yield mv
    finally:
        cql.execute(f"DROP MATERIALIZED VIEW {mv}")

@contextmanager
def new_secondary_index(cql, table, column, name='', extra=''):
    keyspace = table.split('.')[0]
    if not name:
        name = unique_name()
    cql.execute(f"CREATE INDEX {name} ON {table} ({column}) {extra}")
    try:
        yield f"{keyspace}.{name}"
    finally:
        cql.execute(f"DROP INDEX {keyspace}.{name}")

@contextmanager
def new_cql(cql):
    session = cql.cluster.connect()
    try:
        yield session
    finally:
        session.shutdown()

def user_type(*args):
    return collections.namedtuple('user_type', args[::2])(*args[1::2])

@contextmanager
def cql_session(host, port, is_ssl, username, password, request_timeout=120, protocol_version=4):
    from cassandra.cluster import Cluster, ExecutionProfile, EXEC_PROFILE_DEFAULT
    from cassandra.policies import RoundRobinPolicy
    from cassandra.auth import PlainTextAuthProvider
    from cassandra import ConsistencyLevel
    profile = ExecutionProfile(
        load_balancing_policy=RoundRobinPolicy(),
        consistency_level=ConsistencyLevel.ONE,
        request_timeout=request_timeout)
    cluster = Cluster(
        contact_points=[host],
        port=int(port),
        protocol_version=protocol_version,
        execution_profiles={EXEC_PROFILE_DEFAULT: profile},
        auth_provider=PlainTextAuthProvider(username=username, password=password),
        connect_timeout=60,
        control_connection_timeout=60,
    )
    try:
        yield cluster.connect()
    finally:
        cluster.shutdown()

def project(column_name_string, rows):
    return [getattr(r, column_name_string) for r in rows]

def local_process_id(cql):
    return None

class config_value_context:
    def __init__(self, cql, key, value):
        pass
    def __enter__(self):
        pass
    def __exit__(self, *args):
        pass
