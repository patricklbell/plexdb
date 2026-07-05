#include <cql/test_macros/test_macros.h>

#include <string>
#include <vector>

import plexdb.os;
import plexdb.base;

using namespace plexdb;
using namespace plexdb::os;
using namespace cql;

// Regression harness for a heap-buffer-overflow found via cassandra-stress:
// once a single partition-key leaf page accumulates enough rows, node->key_count
// is reconstructed incorrectly and leaf_page() reads phantom slot entries,
// eventually writing past the end of a page. Reproduces with a single connection
// and fully serial requests, so it is a data-volume threshold, not a race.
//
// @note [.] hides this from default runs: at N=20000 it takes 10+ minutes
// (real WAL fsync per transaction, worse under ASan/Debug, and severely worse
// again once the corruption threshold is crossed around row ~14000). Run
// explicitly with a long timeout: cql_tests "[cql.native][bulk]" --skip-benchmarks
CQL_NATIVE_TEST_CASE("bulk insert: many partition keys into a single table", "[cql.native][bulk][.]") {
    constexpr int N = 20000;

    // @note catch2 is not thread safe so we need to capture then check in main thread
    bool        create_ks_ok    = false;
    bool        create_tbl_ok   = false;
    int         first_bad_index = -1;
    std::string first_bad_key;

    test::run_native_server_with_handshake(fixture, [&](Socket& client, Notifier& interrupt) {
        create_ks_ok  = test::send_query(client, "CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': '1'};").opcode == test::op::RESULT;
        create_tbl_ok = test::send_query(client, "CREATE TABLE ks.t (k text PRIMARY KEY, c0 text, c1 text, c2 text, c3 text, c4 text);").opcode == test::op::RESULT;

        uint64_t rng      = 0xdeadbeefcafebabe;
        auto     next_rng = [&]() -> uint64_t {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            return rng;
        };
        static const char alphabet[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        auto              make_key     = [&]() {
            uint64_t    a = next_rng(), b = next_rng();
            std::string key;
            for (int j = 0; j < 8; j++) {
                key += alphabet[(a >> (j * 8)) % 36];
            }
            key += alphabet[b % 36];
            key += alphabet[(b >> 8) % 36];
            return key;
        };

        std::vector<std::string> keys(N);
        for (int i = 0; i < N; i++) {
            keys[i] = make_key();
        }

        for (int i = 0; i < N && first_bad_index < 0; i++) {
            std::string q  = "INSERT INTO ks.t (k, c0, c1, c2, c3, c4) VALUES ('" + keys[i] + "', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb', 'cccccccccccccccccccccccccccccccccc', 'dddddddddddddddddddddddddddddddddd', 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee');";
            test::Frame fr = test::send_query(client, String8(q.c_str(), q.size()));
            if (fr.opcode != test::op::RESULT) {
                first_bad_index = i;
                first_bad_key   = keys[i];
                break;
            }
        }

        signal_notify_safe(interrupt);
    });

    CHECK(create_ks_ok);
    CHECK(create_tbl_ok);
    INFO("first failing row index=" << first_bad_index << " key=" << first_bad_key);
    REQUIRE(first_bad_index < 0);
    co_return;
}
