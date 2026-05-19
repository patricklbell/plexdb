#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <algorithm>
#include <cstring>
#include <vector>
#include <numeric>

#include <plexdb/test_macros/test_macros.h>

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.btree;
import plexdb.btree.print;
import plexdb.btree.types;

using namespace plexdb;
using namespace plexdb::btree;

PAGER_TEST_CASE("insert", "[plexdb.btree.in-memory]") {
    SECTION("(internal=3,leaf=4) consecutive insertion") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);

            for (int i = 0; i <= key; i++) {
                REQUIRE(*co_await tfind<int>(t, i) == 10*i);
            }
        }
    }

    SECTION("(internal=7,leaf=5) consecutive insertion") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 144u);

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);

            for (int i = 0; i <= key; i++) {
                REQUIRE(*co_await tfind<int>(t, i) == 10*i);
            }
        }
    }


    SECTION("consecutive insertion with combinations of tree settings") {
        for (int max_leaf = 3; max_leaf <= 5; max_leaf++) {
            for (int max_internal = 3; max_internal <= 5; max_internal++) {
                U32 nb = static_cast<U32>(std::max(
                    sizeof(Node) + max_internal * sizeof(U64) + (max_internal + 1) * sizeof(NodeRef),
                    sizeof(Node) + max_leaf * (sizeof(U64) + sizeof(int))
                ));
                BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, nb);

                for (int key = 0; key < max_leaf*max_internal+1; key++) {
                    int value = 10*key;
                    co_await tinsert(t, key, value);

                    for (int i = 0; i <= key; i++) {
                        REQUIRE(*co_await tfind<int>(t, i) == 10*i);
                    }
                }
            }
        }
    }

    SECTION("(internal=5,leaf=3) duplicate key insertion overwrites value") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 112u);

        for (int key = 0; key < 5; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
        }

        for (int key = 0; key < 5; key++) {
            int new_value = 100 * key;
            co_await tinsert(t, key, new_value);
        }

        for (int key = 0; key < 5; key++) {
            REQUIRE(*co_await tfind<int>(t, key) == 100 * key);
        }
    }

    SECTION("(internal=4,leaf=4) reverse insertion order") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 96u);

        for (int key = 31; key >= 0; key--) {
            int value = 10*key;
            co_await tinsert(t, key, value);
        }

        // INFO(drive_test_pager(to_str(create_tag<int>(&t))));

        for (int key = 0; key < 32; key++) {
            REQUIRE(*co_await tfind<int>(t, key) == 10*key);
        }
    }

    SECTION("(internal=3,leaf=3) different value sizes and inhomogeneous inserts") {
        struct {
            int key;
            std::vector<U8> value;
        } entries[] = {
            {0, {1, 2, 3, 4}},
            {1, {9}},
            {2, {5, 6, 7, 8, 9}},
            {3, {0}},
            {4, {42, 43}},
        };

        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{}, 80u);

        U64 max_value_length = 0;
        for (auto& e : entries) {
            co_await insert(t, e.key, {e.value.data(), (U16)e.value.size()});
            max_value_length = max(max_value_length, e.value.size());
        }

        std::vector<U8> tmp;
        tmp.reserve(max_value_length);

        for (auto& e : entries) {
            tmp.resize(e.value.size());
            REQUIRE(co_await find(t, e.key, tmp.data(), (U16)tmp.size()));
            REQUIRE(tmp == e.value);
        }

        for (auto& e : entries) {
            co_await remove(t, e.key);
            for (auto& f : entries) {
                if (f.key != e.key && f.key > e.key) {
                    tmp.resize(f.value.size());
                    REQUIRE(co_await find(t, f.key, tmp.data(), (U16)tmp.size()));
                    REQUIRE(tmp == f.value);
                }
            }
        }
    }

    SECTION("varlen key and value basic insert, find, and remove") {
        BTreeInMemory<VarlenKeyPolicy<>, VarlenValuePolicy> t(
            VarlenKeyPolicy<>{}, VarlenValuePolicy{}, 256u);

        auto k = [](const char* s) {
            return TArrayView<const U8, U16>{reinterpret_cast<const U8*>(s), static_cast<U16>(strlen(s))};
        };

        co_await insert(t, k("apple"),  k("fruit"));
        co_await insert(t, k("banana"), k("yellow fruit"));
        co_await insert(t, k("cherry"), k("red fruit"));

        U8 buf[64]; U16 sz;
        REQUIRE( co_await find(t, k("apple"),  buf, sizeof(buf), &sz));
        REQUIRE(std::equal(buf, buf + sz, reinterpret_cast<const U8*>("fruit")));

        REQUIRE( co_await find(t, k("banana"), buf, sizeof(buf), &sz));
        REQUIRE(std::equal(buf, buf + sz, reinterpret_cast<const U8*>("yellow fruit")));

        REQUIRE(!co_await find(t, k("grape"),  buf, sizeof(buf)));

        co_await remove(t, k("banana"));
        REQUIRE(!co_await find(t, k("banana"), buf, sizeof(buf)));
        REQUIRE( co_await find(t, k("apple"),  buf, sizeof(buf), &sz));
        REQUIRE(std::equal(buf, buf + sz, reinterpret_cast<const U8*>("fruit")));
    }

    BENCHMARK_ADVANCED("(internal=5,leaf=7) key with same value")(Catch::Benchmark::Chronometer meter) {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 112u);

        meter.measure([&t] (int i) {
            drive_test_pager(tinsert(t, i, i));
        });
    };
}

PAGER_TEST_CASE("remove", "[plexdb.btree.in-memory]") {
    SECTION("(internal=3,leaf=4) consecutive deletion in order") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
        }

        for (int key = 0; key < 32; key++) {
            co_await remove(t, key);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
            for (int i = key+1; i < 32; i++) {
                REQUIRE(*co_await tfind<int>(t, i) == 10*i);
            }
        }
    }

    SECTION("(internal=4,leaf=3) reverse deletion order") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 96u);

        for (int key = 0; key < 16; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
        }

        for (int key = 15; key >= 0; key--) {
            co_await remove(t, key);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
            for (int i = 0; i < key; i++) {
                REQUIRE(*co_await tfind<int>(t, i) == 10*i);
            }
        }
    }

    SECTION("consecutive deletion in order with combinations of tree settings") {
        for (int max_leaf = 3; max_leaf <= 5; max_leaf++) {
            for (int max_internal = 3; max_internal <= 5; max_internal++) {
                U32 nb = static_cast<U32>(std::max(
                    sizeof(Node) + max_internal * sizeof(U64) + (max_internal + 1) * sizeof(NodeRef),
                    sizeof(Node) + max_leaf * (sizeof(U64) + sizeof(int))
                ));
                BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, nb);

                for (int key = 0; key < max_leaf*max_internal*max_internal; key++) {
                    int value = 10*key;
                    co_await tinsert(t, key, value);
                    // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
                }

                for (int key = 0; key < max_leaf*max_internal*max_internal; key++) {
                    co_await remove(t, key);
                    // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
                    for (int i = key+1; i < max_leaf*max_internal*max_internal; i++) {
                        REQUIRE(*co_await tfind<int>(t, i) == 10*i);
                    }
                }
            }
        }
    }

    SECTION("(internal=3,leaf=3) consecutive insertion, deletion permutations") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        int kept = 3;
        for (int key = 0; key < kept; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
        }

        for (int max_key = kept; max_key <= 9; max_key++) {
            std::vector<int> elements(max_key-(kept-1));
            std::iota(elements.begin(), elements.end(), kept);

            do {
                // insert in consecutive order
                for (int key = kept; key <= max_key; key++) {
                    int value = 10*key;
                    co_await tinsert(t, key, value);
                    // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
                }

                // remove in permutation order
                for (size_t i = 0; i < elements.size(); i++) {
                    co_await remove(t, elements[i]);
                    // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
                    for (size_t j = i+1; j < elements.size(); j++) {
                        REQUIRE(*co_await tfind<int>(t, elements[j]) == 10*elements[j]);
                    }
                }

                CAPTURE(max_key);
                REQUIRE(t.header.size == static_cast<U64>(kept));
            } while (std::next_permutation(elements.begin(), elements.end()));
        }
    }

    SECTION("(internal=4, leaf=4) reinsertion after complete deletion") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 96u);

        for (int key = 0; key < 8; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
            INFO(drive_test_pager(to_str(create_tag<int>(&t))));
        }

        for (int key = 0; key < 8; key++) {
            co_await remove(t, key);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
        }

        for (int key = 0; key < 8; key++) {
            int value = 100 * key;
            co_await tinsert(t, key, value);
            // INFO(drive_test_pager(to_str(create_tag<int>(&t))));
        }

        for (int key = 0; key < 8; key++) {
            REQUIRE(*co_await tfind<int>(t, key) == 100 * key);
        }
    }
}

PAGER_TEST_CASE("iterate", "[plexdb.btree.in-memory]") {
    SECTION("forward iteration visits all keys in order") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        for (int key = 0; key < 32; key++)
            co_await tinsert(t, key, 10*key);

        auto it  = co_await begin<int>(t);
        auto end_ = end<int>(t);
        int expected_key = 0;
        while (it != end_) {
            REQUIRE(it.key() == static_cast<U64>(expected_key));
            REQUIRE(*it == 10 * expected_key);
            expected_key++;
            co_await it.advance();
        }
        REQUIRE(expected_key == 32);
    }

    SECTION("iteration over empty tree returns end immediately") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        auto it  = co_await begin<int>(t);
        auto end_ = end<int>(t);
        REQUIRE(it == end_);
    }

    SECTION("iteration after removals skips removed keys") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        for (int key = 0; key < 16; key++)
            co_await tinsert(t, key, 10*key);
        for (int key = 0; key < 16; key += 2)
            co_await remove(t, key);

        auto it  = co_await begin<int>(t);
        auto end_ = end<int>(t);
        int expected_key = 1;
        while (it != end_) {
            REQUIRE(it.key() == static_cast<U64>(expected_key));
            REQUIRE(*it == 10 * expected_key);
            expected_key += 2;
            co_await it.advance();
        }
        REQUIRE(expected_key == 17);
    }
}

PAGER_TEST_CASE("truncate", "[plexdb.btree.in-memory]") {
    SECTION("(internal=3,leaf=4) insert then truncate then insert again") {
        BTreeInMemory t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{}, 80u);

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
        }
        REQUIRE(co_await size(t) == 32);

        co_await truncate(t);
        REQUIRE(co_await size(t) == 0);

        for (int key = 0; key < 64; key++) {
            int value = 10*key;
            co_await tinsert(t, key, value);
        }
        REQUIRE(co_await size(t) == 64);
    }
}
