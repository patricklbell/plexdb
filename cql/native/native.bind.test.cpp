#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

#include <plexdb/macros/macros.h>
#include <plexdb/test_macros/test_macros.h>
#include <cql/test_macros/test_macros.h>

import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.test.pager_helpers;
import plexdb.aio;
import plexdb.test.server_helpers;

import cql.engine;
import cql.native;
import cql.test_helpers;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;
using namespace cql::column_types;
using namespace cql::test;

namespace {
    constexpr S32 ERR_INVALID = 0x2200;

    DynamicArray<U8> int_value(S32 x) {
        DynamicArray<U8> v;
        append_cql_bind_s32(v, x);
        return v;
    }

    DynamicArray<U8> text_value(String8 s) {
        DynamicArray<U8> v;
        append_cql_bind_str(v, s);
        return v;
    }

    DynamicArray<U8> length_only(S32 n) {
        DynamicArray<U8> v;
        append_be_s32(v, n);
        return v;
    }

    void append_values(DynamicArray<U8>& b, const DynamicArray<DynamicArray<U8>>& values) {
        append_be_u16(b, 0x0001);
        append_u8(b, values.length ? 0x01 : 0x00);
        if (values.length) {
            append_be_u16(b, U16(values.length));
            for (const auto& v : values) {
                append_bytes(b, v.ptr, v.length);
            }
        }
    }

    Frame execute(Socket& s, TArrayView<const U8> id, const DynamicArray<DynamicArray<U8>>& values) {
        DynamicArray<U8> b;
        append_cql_short_bytes(b, id.ptr, U16(id.length));
        append_values(b, values);
        prepend_frame_header(b, op::EXECUTE, 0, 4);
        send_frame(s, b);
        return recv_frame(s);
    }

    Frame query(Socket& s, String8 cql, const DynamicArray<DynamicArray<U8>>& values) {
        DynamicArray<U8> b;
        append_cql_long_string(b, cql);
        append_values(b, values);
        prepend_frame_header(b, op::QUERY, 0, 4);
        send_frame(s, b);
        return recv_frame(s);
    }

    bool appears_in_order(const Frame& f, std::initializer_list<String8> needles) {
        U64 last = 0;
        for (String8 n : needles) {
            U64 at = body_index_of(f, n);
            if (at == MAX_U64 || at < last) {
                return false;
            }
            last = at;
        }
        return true;
    }
}

CQL_NATIVE_TEST_CASE("Bind markers are numbered in query-text order with schema types", "[cql.native][bind]") {
    run_native_server_with_handshake(fixture, [](Socket& s, Notifier& interrupt) {
        CHECK(send_query(s, "CREATE KEYSPACE b;").opcode == op::RESULT);
        CHECK(send_query(s, "CREATE TABLE b.t (pkey int PRIMARY KEY, val text, mp map<text, int>, lst list<int>);").opcode == op::RESULT);

        // DELETE binds its column subscripts before USING, which precedes WHERE.
        Frame del = send_prepare(s, "DELETE mp[?] FROM b.t USING TIMESTAMP ? WHERE pkey = ?;");
        REQUIRE(result_kind(del) == result::PREPARED);
        CHECK(appears_in_order(del, {"key(mp)", "[timestamp]", "pkey"}));

        Frame sel = send_prepare(s, "SELECT val FROM b.t WHERE pkey = ? LIMIT ?;");
        REQUIRE(result_kind(sel) == result::PREPARED);
        CHECK(appears_in_order(sel, {"pkey", "[limit]"}));

        Frame named = send_prepare(s, "UPDATE b.t SET val = :newval WHERE pkey = :which;");
        CHECK(appears_in_order(named, {"newval", "which"}));

        // UPDATE: USING, then SET, then WHERE.
        Frame upd = send_prepare(s, "UPDATE b.t USING TTL ? SET val = ? WHERE pkey = ?;");
        REQUIRE(result_kind(upd) == result::PREPARED);
        CHECK(appears_in_order(upd, {"[ttl]", "val", "pkey"}));
        {
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(3600));
            push_back(values, text_value("ttl-row"));
            push_back(values, int_value(5));
            CHECK(execute(s, read_prepared_id(upd), values).opcode == op::RESULT);
            CHECK(body_contains(send_query(s, "SELECT val FROM b.t WHERE pkey = 5;"), "ttl-row"));
        }

        // Markers nested in collection literals are typed by element.
        Frame ins = send_prepare(s, "INSERT INTO b.t (pkey, lst, mp) VALUES (?, [?, ?], {?: ?});");
        REQUIRE(result_kind(ins) == result::PREPARED);
        {
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, int_value(10));
            push_back(values, int_value(20));
            push_back(values, text_value("nested-key"));
            push_back(values, int_value(30));
            CHECK(execute(s, read_prepared_id(ins), values).opcode == op::RESULT);
            CHECK(body_contains(send_query(s, "SELECT mp FROM b.t WHERE pkey = 1;"), "nested-key"));
        }

        // IN (?, ?) binds each element with the column type.
        {
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(1));
            push_back(values, int_value(5));
            Frame f = query(s, "SELECT val FROM b.t WHERE pkey IN (?, ?);", values);
            CHECK(result_kind(f) == result::ROWS);
            CHECK(body_contains(f, "ttl-row"));
        }

        // Bound LIMIT: UNSET means no limit, null is invalid.
        {
            DynamicArray<DynamicArray<U8>> values;
            push_back(values, int_value(5));
            push_back(values, length_only(-2));
            CHECK(result_kind(execute(s, read_prepared_id(sel), values)) == result::ROWS);

            DynamicArray<DynamicArray<U8>> nulls;
            push_back(nulls, int_value(5));
            push_back(nulls, length_only(-1));
            CHECK(result_kind(execute(s, read_prepared_id(sel), nulls)) == ERR_INVALID);
        }

        // Markers whose type cannot be inferred are rejected instead of guessed.
        Frame unknown_col = send_prepare(s, "SELECT val FROM b.t WHERE nope = ?;");
        CHECK(result_kind(unknown_col) == ERR_INVALID);
        CHECK(body_contains(unknown_col, "nope"));
        Frame fn_arg = send_prepare(s, "INSERT INTO b.t (pkey, val) VALUES (?, blobAsText(?));");
        CHECK(result_kind(fn_arg) == ERR_INVALID);
        signal_notify_safe(interrupt);
    });
    co_return;
}
