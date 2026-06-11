#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

#include <plexdb/test_macros/test_macros.h>

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.btree;
import plexdb.btree.print;
import plexdb.pager;
import plexdb.pager.test_helpers;

using namespace plexdb;
using namespace plexdb::btree;

PAGER_TEST_CASE("insert", "[plexdb.btree.paged]") {
    SECTION("(page_size=128) consecutive insertion") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++) {
                co_await tinsert(t, key, 10*key);

                for (int i = 0; i <= key; i++)
                    REQUIRE(*co_await tfind<int>(t, i) == 10*i);
            }
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 10*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) duplicate key insertion overwrites value") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 5; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 5; key++) co_await tinsert(t, key, 100*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 5; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 100*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reverse insertion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 31; key >= 0; key--) co_await tinsert(t, key, 10*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 10*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) different value sizes and inhomogeneous inserts") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        struct Entry {
            int key;
            std::vector<U8> value;
        };

        Entry entries[] = {
            {0, {1,2,3,4}},
            {1, {9}},
            {2, {5,6,7,8,9}},
            {3, {0}},
            {4, {42,43}},
        };

        {
            auto pager = create_test_pager(pfile, 256_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{});

            for (auto& e : entries) {
                co_await insert(t, e.key, {e.value.data(), (U16)e.value.size()});
            }
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{});

            std::vector<U8> tmp(9);
            for (auto& e : entries) {
                // partial insert zero-fills the remaining stride bytes
                std::vector<U8> expected(9, 0);
                std::copy(e.value.begin(), e.value.end(), expected.begin());
                REQUIRE(co_await find(t, e.key, tmp.data(), (U16)tmp.size()));
                REQUIRE(tmp == expected);
            }
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) split routing: update on full-leaf median key") {
        // Layout with page_size=128, U64 key (8B), int value (4B):
        //   sizeof(Node)=24, capacity = (128-24)/12 = 8 entries/leaf
        //   split_index m = (8+1)/2 = 4, sep = keys[4]
        //
        // After inserting 0..11:
        //   root=[4], left=[0..3], right=[4..11]  (right is full)
        // Updating key=8 (right.keys[4]) triggers the split with sep=8.
        os::File pfile(os::file_tmp());

        Optional<int> found_val;
        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            U64 hp = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, hp, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key <= 11; key++)
                co_await tinsert(t, key, 10*key);

            co_await tinsert(t, 8, 999);   // update key 8 (split median of full right leaf)
            found_val = co_await tfind<int>(t, 8);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        REQUIRE(found_val.has_value());
        REQUIRE(*found_val == 999);
    }

    SECTION("varlen key + fixed value (16B): 200 inserts in separate transactions, all values verified") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        using PartBTree = BTreePaged<VarlenKeyPolicy<>, FixedValuePolicy<16>>;

        constexpr int N = 200;
        struct Entry { uint8_t key[10]; uint8_t val[16]; };
        std::vector<Entry> entries(N);

        // generate deterministic pseudo-random 10-byte keys and 16-byte values
        uint64_t rng = 0xdeadbeefcafebabe;
        auto next_rng = [&]() -> uint64_t {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            return rng;
        };
        for (int i = 0; i < N; i++) {
            uint64_t a = next_rng(), b = next_rng();
            for (int j = 0; j < 8; j++) entries[i].key[j] = uint8_t(a >> (j*8));
            entries[i].key[8] = uint8_t(b); entries[i].key[9] = uint8_t(b >> 8);
            uint64_t c = next_rng(), d = next_rng();
            for (int j = 0; j < 8; j++) entries[i].val[j]   = uint8_t(c >> (j*8));
            for (int j = 0; j < 8; j++) entries[i].val[8+j] = uint8_t(d >> (j*8));
        }

        // create btree
        {
            auto pager = create_test_pager(pfile, 4096_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        // insert each entry in its own transaction (matches engine behaviour)
        // also do a read-back after each insert to detect corruption early
        for (int i = 0; i < N; i++) {
            {
                auto pager = test_pager(pfile);
                pager::Transaction tx{&pager};
                co_await tx.begin();
                PartBTree t(&pager, header_page, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});
                TArrayView<const U8, U16> k{entries[i].key, 10};
                TArrayView<const U8, U16> v{entries[i].val, 16};
                co_await insert(t, k, v);
                co_await tx.commit();
                destroy_test_pager(pager);
            }
            // verify this entry is findable immediately after insert
            {
                auto pager = test_pager(pfile);
                pager::Transaction tx{&pager};
                co_await tx.begin();
                PartBTree t(&pager, header_page, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});
                TArrayView<const U8, U16> k{entries[i].key, 10};
                uint8_t buf[16]; uint16_t sz = 0;
                bool found = co_await find(t, k, buf, 16, &sz);
                auto cur_size = co_await btree::size(t);
                INFO("after insert " << i << " key[0]=" << (int)entries[i].key[0] << " size=" << cur_size);
                REQUIRE(found);
                co_await tx.commit();
                destroy_test_pager(pager);
            }
        }

        // verify every entry
        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            PartBTree t(&pager, header_page, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});
            U64 total_size = co_await btree::size(t);
            REQUIRE(total_size == (U64)N);
            for (int i = 0; i < N; i++) {
                TArrayView<const U8, U16> k{entries[i].key, 10};
                uint8_t buf[16]; uint16_t sz = 0;
                bool found = co_await find(t, k, buf, 16, &sz);
                INFO("entry " << i << " key[0]=" << (int)entries[i].key[0]);
                REQUIRE(found);
                REQUIRE(sz == 16);
                REQUIRE(std::equal(buf, buf + 16, entries[i].val));
            }
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("compact() regression: varlen leaf upward-move corruption") {
        // @note upward compact() aliases source data of a higher-addressed slot; trigger: insert [2,1,3,4,5,6,0,7,(1,128),(1,200)] page_size=256
        os::File pfile(os::file_tmp());
        U64 header_page;
        using PartBTree = BTreePaged<VarlenKeyPolicy<>, FixedValuePolicy<16>>;

        auto k = [](uint8_t a, uint8_t b = 0) {
            std::array<uint8_t,10> key{}; key[0] = a; key[1] = b; return key;
        };
        auto v = [](uint8_t fill) {
            std::array<uint8_t,16> val; val.fill(fill); return val;
        };

        using E = std::pair<std::array<uint8_t,10>, std::array<uint8_t,16>>;
        // Insert order puts key 2 at the highest physical address and key 0 at
        // the lowest; after splits, compact() is triggered on a leaf containing
        E entries[] = {
            {k(2),     v(20)},  // 1st insert → highest physical addr, stays in left
            {k(1),     v(10)},
            {k(3),     v(30)},
            {k(4),     v(40)},
            {k(5),     v(50)},
            {k(6),     v(60)},
            {k(0),     v(0)},   // 7th insert → lowest physical addr, stays in left
            {k(7),     v(70)},  // triggers first split: left=[0..3], right=[4..7]
            {k(1,128), v(15)},  // fills left to 8B free
            {k(1,200), v(17)},  // triggers split of left, then compact() on left2
        };
        constexpr int N = 10;

        {
            auto pager = create_test_pager(pfile, 256_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});
            PartBTree t(&pager, header_page, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});

            for (auto& [key, val] : entries) {
                TArrayView<const U8, U16> kv{key.data(), 10};
                TArrayView<const U8, U16> vv{val.data(), 16};
                co_await insert(t, kv, vv);
            }
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            PartBTree t(&pager, header_page, VarlenKeyPolicy<>{}, FixedValuePolicy<16>{});

            REQUIRE(co_await btree::size(t) == (U64)N);
            for (auto& [key, val] : entries) {
                TArrayView<const U8, U16> kv{key.data(), 10};
                uint8_t buf[16]; uint16_t sz = 0;
                bool found = co_await find(t, kv, buf, 16, &sz);
                INFO("key[0]=" << (int)key[0] << " key[1]=" << (int)key[1]);
                REQUIRE(found);
                REQUIRE(sz == 16);
                REQUIRE(std::equal(buf, buf + 16, val.data()));
            }
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("varlen key and value insert, persist, and reload") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 256_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, VarlenKeyPolicy<>{}, VarlenValuePolicy{});
            BTreePaged<VarlenKeyPolicy<>, VarlenValuePolicy> t(&pager, header_page, VarlenKeyPolicy<>{}, VarlenValuePolicy{});

            auto k = [](const char* s) {
                return TArrayView<const U8, U16>{reinterpret_cast<const U8*>(s), static_cast<U16>(strlen(s))};
            };

            co_await insert(t, k("foo"),   k("bar"));
            co_await insert(t, k("hello"), k("world"));
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged<VarlenKeyPolicy<>, VarlenValuePolicy> t(&pager, header_page, VarlenKeyPolicy<>{}, VarlenValuePolicy{});

            auto k = [](const char* s) {
                return TArrayView<const U8, U16>{reinterpret_cast<const U8*>(s), static_cast<U16>(strlen(s))};
            };

            U8 buf[64]; U16 sz;
            REQUIRE( co_await find(t, k("foo"),     buf, sizeof(buf), &sz));
            REQUIRE(std::equal(buf, buf + sz, reinterpret_cast<const U8*>("bar")));

            REQUIRE( co_await find(t, k("hello"),   buf, sizeof(buf), &sz));
            REQUIRE(std::equal(buf, buf + sz, reinterpret_cast<const U8*>("world")));

            REQUIRE(!co_await find(t, k("missing"), buf, sizeof(buf)));
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }
}

PAGER_TEST_CASE("remove", "[plexdb.btree.paged]") {
    SECTION("(page_size=128) consecutive deletion in order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 32; key++) co_await remove(t, key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            for (int key = 0; key < 32; key++)
                REQUIRE(co_await find(t, key, nullptr, 0) == false);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reverse deletion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 16; key++) co_await tinsert(t, key, 10*key);
            for (int key = 15; key >= 0; key--) co_await remove(t, key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            for (int key = 0; key < 16; key++)
                REQUIRE(co_await find(t, key, nullptr, 0) == false);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reinsertion after complete deletion") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 8; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 8; key++) co_await remove(t, key);
            for (int key = 0; key < 8; key++) co_await tinsert(t, key, 100*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 8; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 100*key);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }
}


PAGER_TEST_CASE("truncate", "[plexdb.btree.paged]") {
    SECTION("(page_size=128) insert then truncate and check pages are freed") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 1000; key++) co_await tinsert(t, key, 10*key);
            REQUIRE(co_await size(t) == 1000);

            co_await truncate(t);
            REQUIRE(co_await size(t) == 0);
            co_await tx.commit();
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::Transaction tx{&pager};
            co_await tx.begin();
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            REQUIRE(co_await size(t) == 0);
            co_await tx.commit();
            destroy_test_pager(pager);
        }
    }
}
