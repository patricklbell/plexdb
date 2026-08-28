# Ported from Apache Cassandra's test/unit/org/apache/cassandra/cql3/validation/
# miscellaneous/PgStringTest.java (no equivalent exists in ScyllaDB's cqlpy port).
# Exercises postgres-style dollar-quoted string literals ($$...$$ / $tag$...$tag$).
#
# SPDX-License-Identifier: Apache-2.0

from ...porting import *

def testPgStyleFunction(cql, test_keyspace):
    execute(cql, test_keyspace, "create or replace function %s." + unique_name() + " ( input double ) called on null input returns text language java\n"
            "AS $$return \"foobar\";$$")

def testPgStyleInsert(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(key ascii PRIMARY KEY, val text)") as table:
        # Some non-terminated pg-strings
        assert_invalid_syntax(cql, table, "INSERT INTO %s (key, val) VALUES ($ $key_empty$$, $$'' value for empty$$)")
        assert_invalid_syntax(cql, table, "INSERT INTO %s (key, val) VALUES ($$key_empty$$, $$'' value for empty$ $)")
        assert_invalid_syntax(cql, table, "INSERT INTO %s (key, val) VALUES ($$key_empty$ $, $$'' value for empty$$)")

        # Different pg-style markers for multiple strings
        execute(cql, table, "INSERT INTO %s (key, val) VALUES ($$prim$ $ $key$$, $$some '' arbitrary value$$)")
        # Same empty pg-style marker for multiple strings
        execute(cql, table, "INSERT INTO %s (key, val) VALUES ($$key_empty$$, $$'' value for empty$$)")
        # Strange but valid pg-style
        execute(cql, table, "INSERT INTO %s (key, val) VALUES ($$$foo$_$foo$$, $$$'' value for empty$$)")
        # These are conventional quoted strings
        execute(cql, table, "INSERT INTO %s (key, val) VALUES ('$txt$key$$$$txt$', '$txt$'' other value$txt$')")

        assert_rows(execute(cql, table, "SELECT key, val FROM %s WHERE key='prim$ $ $key'"),
            row("prim$ $ $key", "some '' arbitrary value"))
        assert_rows(execute(cql, table, "SELECT key, val FROM %s WHERE key='key_empty'"),
            row("key_empty", "'' value for empty"))
        assert_rows(execute(cql, table, "SELECT key, val FROM %s WHERE key='$foo$_$foo'"),
            row("$foo$_$foo", "$'' value for empty"))
        assert_rows(execute(cql, table, "SELECT key, val FROM %s WHERE key='$txt$key$$$$txt$'"),
            row("$txt$key$$$$txt$", "$txt$' other value$txt$"))

        # Invalid syntax: mismatched pg-style tags
        assert_invalid_syntax(cql, table, "INSERT INTO %s (key, val) VALUES ($ascii$prim$$$key$invterm$, $txt$some '' arbitrary value$txt$)")

def testMarkerPgFail(cql, test_keyspace):
    assert_invalid_syntax(cql, test_keyspace,
        "create function %s." + unique_name() + " ( input double ) called on null input returns bigint language java\n"
        "AS $javasrc$return 0L;$javasrc$;")
