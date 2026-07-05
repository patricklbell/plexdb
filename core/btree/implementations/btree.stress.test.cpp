#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <map>
#include <vector>

#include <plexdb/test_macros/test_macros.h>

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.btree;
import plexdb.pager;
import plexdb.test.pager_helpers;

using namespace plexdb;
using namespace plexdb::btree;

namespace {
    struct Rng {
        U64 state;

        explicit Rng(U64 seed)
            : state(seed) {
        }

        U64 next() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }

        U64 below(U64 n) {
            return next() % n;
        }
    };

    enum class Op {
        Insert,
        Remove,
        Find
    };

    Op pick_op(Rng& rng, bool oracle_empty) {
        if (oracle_empty) {
            return Op::Insert;
        }
        U64 r = rng.below(10);
        if (r < 5) {
            return Op::Insert;
        }
        if (r < 8) {
            return Op::Find;
        }
        return Op::Remove;
    }
}

PAGER_TEST_CASE("btree stress", "[plexdb.btree.stress]") {
    SECTION("in-memory fixed U64 key/value: randomized insert/remove/find matches oracle") {
        U32 node_sizes[] = {80, 96, 112, 128, 144, 200, 256};
        for (U32 node_size : node_sizes) {
            CAPTURE(node_size);
            BTreeInMemory      t(FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(U64)>{}, node_size);
            std::map<U64, U64> oracle;
            Rng                rng{0x9e3779b97f4a7c15ull ^ node_size};

            constexpr int key_space = 400;
            constexpr int steps     = 3000;
            for (int step = 0; step < steps; step++) {
                Op  op = pick_op(rng, oracle.empty());
                U64 k  = rng.below(key_space);

                if (op == Op::Insert) {
                    U64 v = rng.next();
                    co_await tinsert(t, k, v);
                    oracle[k] = v;
                } else if (op == Op::Remove) {
                    auto it = oracle.begin();
                    std::advance(it, rng.below(static_cast<U64>(oracle.size())));
                    U64 rk = it->first;
                    oracle.erase(it);
                    bool removed = co_await remove(t, rk);
                    REQUIRE(removed);
                } else {
                    auto found    = co_await tfind<U64>(t, k);
                    auto oit      = oracle.find(k);
                    bool expected = oit != oracle.end();
                    CAPTURE(step, k);
                    REQUIRE(found.has_value() == expected);
                    if (expected) {
                        REQUIRE(*found == oit->second);
                    }
                }
            }

            REQUIRE(co_await size(t) == static_cast<U64>(oracle.size()));
            for (auto& [k, v] : oracle) {
                auto found = co_await tfind<U64>(t, k);
                CAPTURE(k);
                REQUIRE(found.has_value());
                REQUIRE(*found == v);
            }

            auto it        = co_await begin<U64>(t);
            auto end_      = end<U64>(t);
            auto oracle_it = oracle.begin();
            while (it != end_) {
                REQUIRE(oracle_it != oracle.end());
                REQUIRE(it.key() == oracle_it->first);
                REQUIRE(*it == oracle_it->second);
                ++oracle_it;
                co_await it.advance();
            }
            REQUIRE(oracle_it == oracle.end());
        }
    }

    SECTION("in-memory varlen key/value: randomized insert/remove/find matches oracle") {
        // Node sizes chosen so a full node can always split into two non-empty
        // halves (and, for internal nodes, still extract a separator) even at
        // max key/value length; smaller sizes can produce a full node with only
        // 1-2 entries, which no split point can satisfy — the same class of
        // "entry too large for node" limitation tracked in btree/TODO.md under
        // overflow pages, not something this test is meant to exercise.
        U32 node_sizes[] = {160, 200, 256, 320};
        for (U32 node_size : node_sizes) {
            CAPTURE(node_size);
            BTreeInMemory<VarlenKeyPolicy<>, VarlenValuePolicy<>> t(VarlenKeyPolicy<>{}, VarlenValuePolicy<>{}, node_size);
            std::map<std::vector<U8>, std::vector<U8>>            oracle;
            Rng                                                   rng{0xbf58476d1ce4e5b9ull ^ node_size};

            auto random_bytes = [&](U64 max_len) {
                std::vector<U8> bytes(1 + rng.below(max_len));
                for (auto& b : bytes) {
                    b = static_cast<U8>(rng.next());
                }
                return bytes;
            };

            constexpr int steps = 2000;
            for (int step = 0; step < steps; step++) {
                Op op = pick_op(rng, oracle.empty());

                if (op == Op::Insert) {
                    std::vector<U8> key   = random_bytes(8);
                    std::vector<U8> value = random_bytes(16);
                    co_await insert(t, {key.data(), static_cast<U16>(key.size())}, {value.data(), static_cast<U16>(value.size())});
                    oracle[key] = value;
                } else if (op == Op::Remove) {
                    auto it = oracle.begin();
                    std::advance(it, rng.below(static_cast<U64>(oracle.size())));
                    std::vector<U8> rk = it->first;
                    oracle.erase(it);
                    bool removed = co_await remove(t, {rk.data(), static_cast<U16>(rk.size())});
                    REQUIRE(removed);
                } else {
                    std::vector<U8> key = random_bytes(8);
                    U8              buf[32];
                    U16             sz       = 0;
                    bool            found    = co_await find(t, {key.data(), static_cast<U16>(key.size())}, buf, sizeof(buf), &sz);
                    auto            oit      = oracle.find(key);
                    bool            expected = oit != oracle.end();
                    CAPTURE(step);
                    REQUIRE(found == expected);
                    if (expected) {
                        REQUIRE(sz == oit->second.size());
                        REQUIRE(std::equal(buf, buf + sz, oit->second.data()));
                    }
                }
            }

            REQUIRE(co_await size(t) == static_cast<U64>(oracle.size()));
            for (auto& [k, v] : oracle) {
                U8   buf[32];
                U16  sz    = 0;
                bool found = co_await find(t, {k.data(), static_cast<U16>(k.size())}, buf, sizeof(buf), &sz);
                REQUIRE(found);
                REQUIRE(sz == v.size());
                REQUIRE(std::equal(buf, buf + sz, v.data()));
            }
        }
    }

    SECTION("paged fixed U64 key/value: randomized insert/remove within one transaction matches oracle") {
        U32 page_sizes[] = {128, 200, 256};
        for (U32 page_size : page_sizes) {
            CAPTURE(page_size);
            os::File pfile(os::file_tmp());
            auto     pager = create_test_pager(pfile, static_cast<U64>(page_size));

            pager::Transaction tx{&pager};
            co_await tx.begin();
            U64        header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(U64)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(U64)>{});

            std::map<U64, U64> oracle;
            Rng                rng{0x2545f4914f6cdd1dull ^ page_size};

            constexpr int key_space = 250;
            constexpr int steps     = 1500;
            for (int step = 0; step < steps; step++) {
                Op  op = pick_op(rng, oracle.empty());
                U64 k  = rng.below(key_space);

                if (op == Op::Insert) {
                    U64 v = rng.next();
                    co_await tinsert(t, k, v);
                    oracle[k] = v;
                } else if (op == Op::Remove) {
                    auto it = oracle.begin();
                    std::advance(it, rng.below(static_cast<U64>(oracle.size())));
                    U64 rk = it->first;
                    oracle.erase(it);
                    bool removed = co_await remove(t, rk);
                    REQUIRE(removed);
                } else {
                    auto found    = co_await tfind<U64>(t, k);
                    auto oit      = oracle.find(k);
                    bool expected = oit != oracle.end();
                    CAPTURE(step, k);
                    REQUIRE(found.has_value() == expected);
                    if (expected) {
                        REQUIRE(*found == oit->second);
                    }
                }
            }

            REQUIRE(co_await size(t) == static_cast<U64>(oracle.size()));
            for (auto& [k, v] : oracle) {
                auto found = co_await tfind<U64>(t, k);
                CAPTURE(k);
                REQUIRE(found.has_value());
                REQUIRE(*found == v);
            }

            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }
}
