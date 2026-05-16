#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <coroutine>
#include <algorithm>
#include <vector>
#include <numeric>

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.btree;
import plexdb.btree.print;
import plexdb.aio;

using namespace plexdb;
using namespace plexdb::btree;

static aio::EventConsumer s_sync_consumer{0, aio::OnUnblockFunctor{[](const TArrayView<os::PollEvent>&) -> bool { return true; }}};
static os::Poll s_sync_poll{};

TEST_CASE("insert", "[plexdb.btree.in-memory]" ) {
    SECTION("(internal=3,leaf=4) consecutive insertion") {
        BTreeInMemory t(3, 4, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);

            for (int i = 0; i <= key; i++) {
                REQUIRE(*aio::drive(tfind<int>(t, i), s_sync_consumer, s_sync_poll) == 10*i);
            }
        }
    }

    SECTION("(internal=7,leaf=5) consecutive insertion") {
        BTreeInMemory t(7, 5, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);

            for (int i = 0; i <= key; i++) {
                REQUIRE(*aio::drive(tfind<int>(t, i), s_sync_consumer, s_sync_poll) == 10*i);
            }
        }
    }


    SECTION("consecutive insertion with combinations of tree settings") {
        for (int max_leaf = 3; max_leaf <= 5; max_leaf++) {
            for (int max_internal = 3; max_internal <= 5; max_internal++) {
                BTreeInMemory t(max_internal, max_leaf, sizeof(int));

                for (int key = 0; key < max_leaf*max_internal+1; key++) {
                    int value = 10*key;
                    aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);

                    for (int i = 0; i <= key; i++) {
                        REQUIRE(*aio::drive(tfind<int>(t, i), s_sync_consumer, s_sync_poll) == 10*i);
                    }
                }
            }
        }
    }

    SECTION("(internal=5,leaf=3) duplicate key insertion overwrites value") {
        BTreeInMemory t(5, 3, sizeof(int));

        for (int key = 0; key < 5; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
        }

        for (int key = 0; key < 5; key++) {
            int new_value = 100 * key;
            aio::drive(tinsert(t, key, new_value), s_sync_consumer, s_sync_poll);
        }

        for (int key = 0; key < 5; key++) {
            REQUIRE(*aio::drive(tfind<int>(t, key), s_sync_consumer, s_sync_poll) == 100 * key);
        }
    }

    SECTION("(internal=4,leaf=4) reverse insertion order") {
        BTreeInMemory t(4, 4, sizeof(int));

        for (int key = 31; key >= 0; key--) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
        }

        // INFO(to_str(TBTree<BTreeInMemory,int>(t)));

        for (int key = 0; key < 32; key++) {
            REQUIRE(*aio::drive(tfind<int>(t, key), s_sync_consumer, s_sync_poll) == 10*key);
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

        BTreeInMemory t(3, 3, 9);

        U64 max_value_length = 0;
        for (auto& e : entries) {
            aio::drive(insert(t, e.key, e.value.data(), e.value.size()), s_sync_consumer, s_sync_poll);
            max_value_length = max(max_value_length, e.value.size());
        }

        std::vector<U8> tmp;
        tmp.reserve(max_value_length);

        for (auto& e : entries) {
            tmp.resize(e.value.size());
            REQUIRE(aio::drive(find(t, e.key, tmp.data(), tmp.size()), s_sync_consumer, s_sync_poll));
            REQUIRE(tmp == e.value);
        }

        for (auto& e : entries) {
            aio::drive(remove(t, e.key), s_sync_consumer, s_sync_poll);
            for (auto& f : entries) {
                if (f.key != e.key && f.key > e.key) {
                    tmp.resize(f.value.size());
                    REQUIRE(aio::drive(find(t, f.key, tmp.data(), tmp.size()), s_sync_consumer, s_sync_poll));
                    REQUIRE(tmp == f.value);
                }
            }
        }
    }

    BENCHMARK_ADVANCED("(internal=5,leaf=7) key with same value")(Catch::Benchmark::Chronometer meter) {
        BTreeInMemory t(5, 7, sizeof(int));

        meter.measure([&t] (int i) {
            aio::drive(tinsert(t, i, i), s_sync_consumer, s_sync_poll);
        });
    };
}

TEST_CASE("remove", "[plexdb.btree.in-memory]" ) {
    SECTION("(internal=3,leaf=4) consecutive deletion in order") {
        BTreeInMemory t(3, 4, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
        }

        for (int key = 0; key < 32; key++) {
            aio::drive(remove(t, key), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
            for (int i = key+1; i < 32; i++) {
                REQUIRE(*aio::drive(tfind<int>(t, i), s_sync_consumer, s_sync_poll) == 10*i);
            }
        }
    }

    SECTION("(internal=4,leaf=3) reverse deletion order") {
        BTreeInMemory t(4, 3, sizeof(int));

        for (int key = 0; key < 16; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
        }

        for (int key = 15; key >= 0; key--) {
            aio::drive(remove(t, key), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
            for (int i = 0; i < key; i++) {
                REQUIRE(*aio::drive(tfind<int>(t, i), s_sync_consumer, s_sync_poll) == 10*i);
            }
        }
    }

    SECTION("consecutive deletion in order with combinations of tree settings") {
        for (int max_leaf = 3; max_leaf <= 5; max_leaf++) {
            for (int max_internal = 3; max_internal <= 5; max_internal++) {
                BTreeInMemory t(max_internal, max_leaf, sizeof(int));

                for (int key = 0; key < max_leaf*max_internal*max_internal; key++) {
                    int value = 10*key;
                    aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
                    // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
                }

                for (int key = 0; key < max_leaf*max_internal*max_internal; key++) {
                    aio::drive(remove(t, key), s_sync_consumer, s_sync_poll);
                    // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
                    for (int i = key+1; i < max_leaf*max_internal*max_internal; i++) {
                        REQUIRE(*aio::drive(tfind<int>(t, i), s_sync_consumer, s_sync_poll) == 10*i);
                    }
                }
            }
        }
    }

    SECTION("(internal=3,leaf=3) consecutive insertion, deletion permutations") {
        BTreeInMemory t(3, 3, sizeof(int));

        int kept = 3;
        for (int key = 0; key < kept; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
        }

        for (int max_key = kept; max_key <= 9; max_key++) {
            std::vector<int> elements(max_key-(kept-1));
            std::iota(elements.begin(), elements.end(), kept);

            do {
                // insert in consecutive order
                for (int key = kept; key <= max_key; key++) {
                    int value = 10*key;
                    aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
                    // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
                }

                // remove in permutation order
                for (size_t i = 0; i < elements.size(); i++) {
                    aio::drive(remove(t, elements[i]), s_sync_consumer, s_sync_poll);
                    // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
                    for (size_t j = i+1; j < elements.size(); j++) {
                        REQUIRE(*aio::drive(tfind<int>(t, elements[j]), s_sync_consumer, s_sync_poll) == 10*elements[j]);
                    }
                }

                CAPTURE(max_key);
                REQUIRE(t.header.size == static_cast<U64>(kept));
            } while (std::next_permutation(elements.begin(), elements.end()));
        }
    }

    SECTION("(internal=4, leaf=4) reinsertion after complete deletion") {
        BTreeInMemory t(4, 4, sizeof(int));

        for (int key = 0; key < 8; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
            INFO(to_str(create_tag<int>(&t)));
        }

        for (int key = 0; key < 8; key++) {
            aio::drive(remove(t, key), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
        }

        for (int key = 0; key < 8; key++) {
            int value = 100 * key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
            // INFO(to_str(TBTree<BTreeInMemory,int>(t)));
        }

        for (int key = 0; key < 8; key++) {
            REQUIRE(*aio::drive(tfind<int>(t, key), s_sync_consumer, s_sync_poll) == 100 * key);
        }
    }
}

TEST_CASE("truncate", "[plexdb.btree.in-memory]" ) {
    SECTION("(internal=3,leaf=4) insert then truncate then insert again") {
        BTreeInMemory t(3, 4, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
        }
        REQUIRE(aio::drive(size(t), s_sync_consumer, s_sync_poll) == 32);

        aio::drive(truncate(t), s_sync_consumer, s_sync_poll);
        REQUIRE(aio::drive(size(t), s_sync_consumer, s_sync_poll) == 0);

        for (int key = 0; key < 64; key++) {
            int value = 10*key;
            aio::drive(tinsert(t, key, value), s_sync_consumer, s_sync_poll);
        }
        REQUIRE(aio::drive(size(t), s_sync_consumer, s_sync_poll) == 64);
    }
}
