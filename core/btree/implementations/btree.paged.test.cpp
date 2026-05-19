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
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++) {
                co_await tinsert(t, key, 10*key);

                for (int i = 0; i <= key; i++)
                    REQUIRE(*co_await tfind<int>(t, i) == 10*i);
            }
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 10*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) duplicate key insertion overwrites value") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 5; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 5; key++) co_await tinsert(t, key, 100*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 5; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 100*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reverse insertion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 31; key >= 0; key--) co_await tinsert(t, key, 10*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 10*key);
            co_await pager::commit_transaction(pager);
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
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{});

            for (auto& e : entries) {
                co_await insert(t, e.key, {e.value.data(), (U16)e.value.size()});
            }
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<9>{});

            std::vector<U8> tmp(9);
            for (auto& e : entries) {
                // partial insert zero-fills the remaining stride bytes
                std::vector<U8> expected(9, 0);
                std::copy(e.value.begin(), e.value.end(), expected.begin());
                REQUIRE(co_await find(t, e.key, tmp.data(), (U16)tmp.size()));
                REQUIRE(tmp == expected);
            }
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }
    }

    SECTION("varlen key and value insert, persist, and reload") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 256_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, VarlenKeyPolicy<>{}, VarlenValuePolicy{});
            BTreePaged<VarlenKeyPolicy<>, VarlenValuePolicy> t(&pager, header_page, VarlenKeyPolicy<>{}, VarlenValuePolicy{});

            auto k = [](const char* s) {
                return TArrayView<const U8, U16>{reinterpret_cast<const U8*>(s), static_cast<U16>(strlen(s))};
            };

            co_await insert(t, k("foo"),   k("bar"));
            co_await insert(t, k("hello"), k("world"));
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
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
            co_await pager::commit_transaction(pager);
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
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 32; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 32; key++) co_await remove(t, key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            for (int key = 0; key < 32; key++)
                REQUIRE(co_await find(t, key, nullptr, 0) == false);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reverse deletion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 16; key++) co_await tinsert(t, key, 10*key);
            for (int key = 15; key >= 0; key--) co_await remove(t, key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            for (int key = 0; key < 16; key++)
                REQUIRE(co_await find(t, key, nullptr, 0) == false);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reinsertion after complete deletion") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 8; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 8; key++) co_await remove(t, key);
            for (int key = 0; key < 8; key++) co_await tinsert(t, key, 100*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 8; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 100*key);
            co_await pager::commit_transaction(pager);
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
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            for (int key = 0; key < 1000; key++) co_await tinsert(t, key, 10*key);
            REQUIRE(co_await size(t) == 1000);

            co_await truncate(t);
            REQUIRE(co_await size(t) == 0);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            pager::begin_transaction(pager);
            BTreePaged t(&pager, header_page, FixedKeyPolicy<U64>{}, FixedValuePolicy<sizeof(int)>{});

            REQUIRE(co_await size(t) == 0);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }
    }
}
