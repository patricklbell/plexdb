#include <cql/test_macros/test_macros.h>
#include <catch2/generators/catch_generators.hpp>

#include <cstring>
#include <vector>

import plexdb.os;
import plexdb.base;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

// Diagnostic for a read-path failure surfaced by cassandra-stress: write
// completes cleanly, but the very first read fails "Data returned was not
// validated". cassandra-stress's Java driver negotiates protocol v5, which
// wraps every post-STARTUP message in an outer frame (CRC24 header, CRC32
// payload) that no other test in this repo exercises — every other test
// runs over v4, which has no such wrapping. Parameterized over both
// versions (same body, same fixture-per-run via CQL_NATIVE_TEST_CASE's
// per-invocation ServerFixture) rather than duplicating this test for v5,
// so a v4 pass alongside a v5 failure directly implicates the outer-frame
// codec instead of the data path itself.
CQL_NATIVE_TEST_CASE("blob column round-trip via prepared statements", "[cql.native][blob][.]") {
    U8 version = GENERATE(U8(4), U8(5));
    INFO("protocol version=" << int(version));
    constexpr int N = 20000;

    bool        create_ks_ok         = false;
    bool        create_tbl_ok        = false;
    int         insert_failures      = 0;
    int         mismatch_count       = 0;
    int         first_mismatch_index = -1;
    std::string first_mismatch_detail;

    auto fill_key = [](U8* out, int i) {
        char buf[11];
        snprintf(buf, sizeof(buf), "%010d", i);
        memcpy(out, buf, 10);
    };
    auto fill_col = [](U8* out, int i, U8 tag) {
        for (int j = 0; j < 34; j++) {
            out[j] = U8((i * 7 + j * 13 + tag) & 0xFF);
        }
    };
    // EXECUTE's flags field is a [byte] in v4 but a [int] in v5 — the width
    // itself is protocol-version-dependent, not just the outer framing.
    auto append_flags = [&](DynamicArray<U8>& buf, U32 flags) {
        if (version >= 5) {
            test::append_be_s32(buf, S32(flags));
        } else {
            test::append_u8(buf, U8(flags));
        }
    };
    // v5 EXECUTE carries a result_metadata_id [short bytes] right after the
    // statement id; the server reads and discards it (native.cpp:1054), so
    // an empty value is fine here.
    auto append_execute_id = [&](DynamicArray<U8>& buf, TArrayView<const U8> pid) {
        test::append_cql_short_bytes(buf, pid.ptr, U16(pid.length));
        if (version >= 5) {
            test::append_cql_short_bytes(buf, nullptr, 0);
        }
    };

    test::run_native_server_with_handshake(fixture, [&](Socket& client, Notifier& interrupt) {
        create_ks_ok  = test::send_query(client, "CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': '1'};").opcode == test::op::RESULT;
        create_tbl_ok = test::send_query(client, "CREATE TABLE ks.standard1 (key blob PRIMARY KEY, \"C0\" blob, \"C1\" blob, \"C2\" blob, \"C3\" blob, \"C4\" blob);").opcode == test::op::RESULT;

        test::Frame insert_prep = test::send_prepare(client, "INSERT INTO ks.standard1 (key, \"C0\", \"C1\", \"C2\", \"C3\", \"C4\") VALUES (?, ?, ?, ?, ?, ?)", 1);
        TArrayView<const U8> insert_pid = test::read_prepared_id(insert_prep);

        for (int i = 0; i < N; i++) {
            U8 key[10];
            U8 c0[34], c1[34], c2[34], c3[34], c4[34];
            fill_key(key, i);
            fill_col(c0, i, 0xA0);
            fill_col(c1, i, 0xB0);
            fill_col(c2, i, 0xC0);
            fill_col(c3, i, 0xD0);
            fill_col(c4, i, 0xE0);

            DynamicArray<U8> ex;
            append_execute_id(ex, insert_pid);
            test::append_be_u16(ex, 0x0001);
            append_flags(ex, 0x01);
            test::append_be_u16(ex, 6);
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(key), sizeof(key)));
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(c0), sizeof(c0)));
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(c1), sizeof(c1)));
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(c2), sizeof(c2)));
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(c3), sizeof(c3)));
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(c4), sizeof(c4)));
            test::prepend_frame_header(ex, test::op::EXECUTE, S16(2 + (i % 1000)), version);
            test::send_frame(client, ex);

            test::Frame exec = test::recv_frame(client);
            if (exec.opcode != test::op::RESULT) {
                insert_failures++;
            }
        }

        test::Frame select_prep = test::send_prepare(client, "SELECT * FROM ks.standard1 WHERE key = ?", 30000);
        TArrayView<const U8> select_pid = test::read_prepared_id(select_prep);

        auto contains = [](const DynamicArray<U8>& body, const U8* needle, U64 len) {
            if (body.length < len){ return false;
}
            for (U64 i = 0; i + len <= body.length; i++) {
                if (memcmp(body.ptr + i, needle, len) == 0){ return true;
}
            }
            return false;
        };

        std::vector<int> order(N);
        for (int i = 0; i < N; i++){ order[i] = i;
}
        {
            uint64_t rng = 0x1234567890abcdefULL;
            for (int i = N - 1; i > 0; i--) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                int j = int(rng % uint64_t(i + 1));
                std::swap(order[i], order[j]);
            }
        }

        for (int idx = 0; idx < N; idx++) {
            int i = order[idx];
            U8 key[10];
            U8 c0[34], c1[34], c2[34], c3[34], c4[34];
            fill_key(key, i);
            fill_col(c0, i, 0xA0);
            fill_col(c1, i, 0xB0);
            fill_col(c2, i, 0xC0);
            fill_col(c3, i, 0xD0);
            fill_col(c4, i, 0xE0);

            DynamicArray<U8> ex;
            append_execute_id(ex, select_pid);
            test::append_be_u16(ex, 0x0001);
            append_flags(ex, 0x01);
            test::append_be_u16(ex, 1);
            test::append_cql_bind_str(ex, String8(reinterpret_cast<const char*>(key), sizeof(key)));
            test::prepend_frame_header(ex, test::op::EXECUTE, S16(2 + (idx % 1000)), version);
            test::send_frame(client, ex);

            test::Frame sel = test::recv_frame(client);
            bool ok = sel.opcode == test::op::RESULT && test::result_kind(sel) == test::result::ROWS
                && contains(sel.body, key, sizeof(key))
                && contains(sel.body, c0, sizeof(c0))
                && contains(sel.body, c1, sizeof(c1))
                && contains(sel.body, c2, sizeof(c2))
                && contains(sel.body, c3, sizeof(c3))
                && contains(sel.body, c4, sizeof(c4));
            if (!ok) {
                mismatch_count++;
                if (first_mismatch_index < 0) {
                    first_mismatch_index = i;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "opcode=%d kind=%d body_len=%d", sel.opcode, sel.opcode == test::op::RESULT ? test::result_kind(sel) : -1, sel.body_len);
                    first_mismatch_detail = buf;
                }
            }
        }

        signal_notify_safe(interrupt); }, version);

    CHECK(create_ks_ok);
    CHECK(create_tbl_ok);
    CHECK(insert_failures == 0);
    INFO("first mismatch index=" << first_mismatch_index << " detail=" << first_mismatch_detail);
    INFO("total mismatches=" << mismatch_count << " / " << N);
    CHECK(mismatch_count == 0);
    co_return;
}
