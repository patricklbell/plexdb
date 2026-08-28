# Ported from Apache Cassandra's test/unit/org/apache/cassandra/cql3/validation/
# miscellaneous/OverflowTest.java (no equivalent exists in ScyllaDB's cqlpy port).
#
# SPDX-License-Identifier: Apache-2.0

import math

from ...porting import *

def testNullSupport(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int, c int, v1 int, v2 set<text>, PRIMARY KEY (k, c))") as table:
        execute(cql, table, "INSERT INTO %s (k, c, v1, v2) VALUES (0, 0, null, {'1', '2'})")
        execute(cql, table, "INSERT INTO %s (k, c, v1) VALUES (0, 1, 1)")

        assert_rows(execute(cql, table, "SELECT * FROM %s"),
            row(0, 0, None, {"1", "2"}),
            row(0, 1, 1, None))

        execute(cql, table, "INSERT INTO %s (k, c, v1) VALUES (0, 1, null)")
        execute(cql, table, "INSERT INTO %s (k, c, v2) VALUES (0, 0, null)")

        assert_rows(execute(cql, table, "SELECT * FROM %s"),
            row(0, 0, None, None),
            row(0, 1, None, None))

        assert_invalid(cql, table, "INSERT INTO %s (k, c, v2) VALUES (0, 2, {1, null})")
        assert_invalid(cql, table, "SELECT * FROM %s WHERE k = null")
        assert_invalid(cql, table, "INSERT INTO %s (k, c, v2) VALUES (0, 0, { 'foo', 'bar', null })")

def testReservedKeywords(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(key text PRIMARY KEY, count counter)"):
        pass
    assert_invalid_throw(cql, test_keyspace + "." + unique_name(), SyntaxException,
        "CREATE TABLE %s (select text PRIMARY KEY, x int)")

def testIdentifiers(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(key_23 int PRIMARY KEY, CoLuMn int)") as table:
        execute(cql, table, "INSERT INTO %s (Key_23, Column) VALUES (0, 0)")
        execute(cql, table, "INSERT INTO %s (KEY_23, COLUMN) VALUES (0, 0)")

        assert_invalid(cql, table, "INSERT INTO %s (key_23, column, column) VALUES (0, 0, 0)")
        assert_invalid(cql, table, "INSERT INTO %s (key_23, column, COLUMN) VALUES (0, 0, 0)")
        assert_invalid(cql, table, "INSERT INTO %s (key_23, key_23, column) VALUES (0, 0, 0)")
        assert_invalid(cql, table, "INSERT INTO %s (key_23, KEY_23, column) VALUES (0, 0, 0)")

    assert_invalid_throw(cql, test_keyspace + "." + unique_name(), SyntaxException,
        "CREATE TABLE %s (select int PRIMARY KEY, column int)")

def testTableOptions(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int PRIMARY KEY, c int) WITH "
                "comment = 'My comment' "
                "AND gc_grace_seconds = 4 "
                "AND bloom_filter_fp_chance = 0.01 "
                "AND compaction = { 'class' : 'LeveledCompactionStrategy', 'sstable_size_in_mb' : 10, 'fanout_size' : 5 } "
                "AND compression = { 'enabled': false } "
                "AND caching = { 'keys': 'ALL', 'rows_per_partition': 'ALL' }") as table:
        execute(cql, table, "ALTER TABLE %s WITH "
                "comment = 'other comment' "
                "AND gc_grace_seconds = 100 "
                "AND bloom_filter_fp_chance = 0.1 "
                "AND compaction = { 'class': 'SizeTieredCompactionStrategy', 'min_sstable_size' : 42 } "
                "AND compression = { 'class' : 'SnappyCompressor' } "
                "AND caching = { 'rows_per_partition': 'ALL' }")

def testUnescapedString(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k text PRIMARY KEY, c text)") as table:
        # The apostrophe inside "cassandra's" is not escaped CQL-style ('')
        # so this must be rejected as a syntax error, not silently accepted.
        assert_invalid_throw(cql, table, SyntaxException,
            "INSERT INTO %s (k, c) VALUES ('foo', 'CQL is cassandra's best friend')")

def testBoolean(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k boolean PRIMARY KEY, b boolean)") as table:
        execute(cql, table, "INSERT INTO %s (k, b) VALUES (true, false)")
        assert_rows(execute(cql, table, "SELECT * FROM %s WHERE k = true"),
            row(True, False))

def testFloatWithExponent(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int PRIMARY KEY, d double, f float)") as table:
        execute(cql, table, "INSERT INTO %s (k, d, f) VALUES (0, 3E+10, 3.4E3)")
        execute(cql, table, "INSERT INTO %s (k, d, f) VALUES (1, 3.E10, -23.44E-3)")
        execute(cql, table, "INSERT INTO %s (k, d, f) VALUES (2, 3, -2)")

def testConversionFunctions(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int PRIMARY KEY, i varint, b blob)") as table:
        execute(cql, table, "INSERT INTO %s (k, i, b) VALUES (0, blob_as_varint(bigint_as_blob(3)), text_as_blob('foobar'))")
        assert_rows(execute(cql, table, "SELECT i, blob_as_text(b) FROM %s WHERE k = 0"),
            row(3, "foobar"))

def testEmptyBlob(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int PRIMARY KEY, b blob)") as table:
        execute(cql, table, "INSERT INTO %s (k, b) VALUES (0, 0x)")
        assert_rows(execute(cql, table, "SELECT * FROM %s"),
            row(0, b""))

def testEmpty(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k1 int, k2 int, v int, PRIMARY KEY (k1, k2))") as table:
        # Insert a few rows to make sure we don't actually query something
        for i in range(2):
            for j in range(2):
                execute(cql, table, "INSERT INTO %s (k1, k2, v) VALUES (?, ?, ?)", i, j, i + j)
        rows = list(execute(cql, table, "SELECT * FROM %s"))

        # Test empty IN() in SELECT
        assert_empty(execute(cql, table, "SELECT v FROM %s WHERE k1 IN ()"))
        assert_empty(execute(cql, table, "SELECT v FROM %s WHERE k1 = 0 AND k2 IN ()"))

        # Test empty IN() in DELETE
        execute(cql, table, "DELETE FROM %s WHERE k1 IN ()")
        assert_rows_ignoring_order(execute(cql, table, "SELECT * FROM %s"), *rows)

        # Test empty IN() in UPDATE
        execute(cql, table, "UPDATE %s SET v = 3 WHERE k1 IN () AND k2 = 2")
        assert_rows_ignoring_order(execute(cql, table, "SELECT * FROM %s"), *rows)

def testFunctionWithNull(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int PRIMARY KEY, t timeuuid)") as table:
        execute(cql, table, "INSERT INTO %s (k) VALUES (0)")
        assert_rows(execute(cql, table, "SELECT to_timestamp(t) FROM %s WHERE k=0"),
            row(None))

def testColumnNameValidation(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k text, c int, v timeuuid, PRIMARY KEY (k, c))") as table:
        assert_invalid(cql, table, "INSERT INTO %s (k, c) VALUES ('', 0)")

        # Insert a value that doesn't fit 'int'
        assert_invalid(cql, table, "INSERT INTO %s (k, c) VALUES (0, 10000000000)")

        # Insert a non-version 1 uuid into a timeuuid column
        assert_invalid(cql, table, "INSERT INTO %s (k, c, v) VALUES (0, 0, 550e8400-e29b-41d4-a716-446655440000)")

def testNanInfinityValues(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(f float PRIMARY KEY)") as table:
        execute(cql, table, "INSERT INTO %s (f) VALUES (NaN)")
        execute(cql, table, "INSERT INTO %s (f) VALUES (-NaN)")
        execute(cql, table, "INSERT INTO %s (f) VALUES (Infinity)")
        execute(cql, table, "INSERT INTO %s (f) VALUES (-Infinity)")

        selected = [r[0] for r in execute(cql, table, "SELECT * FROM %s")]

        # NaN and -NaN serialize to the same canonical bit pattern and thus
        # the same partition key, so only 3 distinct rows should remain.
        # Row order is otherwise unspecified (depends on key storage order,
        # not Cassandra's token ring), so check by value, not position.
        assert len(selected) == 3
        assert sum(1 for f in selected if math.isnan(f)) == 1
        assert sum(1 for f in selected if math.isinf(f) and f > 0) == 1
        assert sum(1 for f in selected if math.isinf(f) and f < 0) == 1

def testBlobAsFunction(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(k int PRIMARY KEY, v int)") as table:
        # A blob that is not 4 bytes should be rejected
        assert_invalid(cql, table, "INSERT INTO %s (k, v) VALUES (0, blob_as_int(0x01))")

        execute(cql, table, "INSERT INTO %s (k, v) VALUES (0, blob_as_int(0x00000001))")
        assert_rows(execute(cql, table, "SELECT v FROM %s WHERE k=0"), row(1))
