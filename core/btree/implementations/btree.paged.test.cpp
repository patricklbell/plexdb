#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <algorithm>
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
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

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
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 10*key);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) duplicate key insertion overwrites value") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 5; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 5; key++) co_await tinsert(t, key, 100*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 5; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 100*key);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reverse insertion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 31; key >= 0; key--) co_await tinsert(t, key, 10*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 10*key);
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
            header_page = co_await btree::create_paged(pager, 9);
            BTreePaged t(&pager, header_page);

            for (auto& e : entries) {
                co_await insert(t, e.key, e.value.data(), e.value.size());
            }
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);

            std::vector<U8> tmp;
            tmp.resize(5);
            for (auto& e : entries) {
                tmp.resize(e.value.size());
                REQUIRE(co_await find(t, e.key, tmp.data(), tmp.size()));
                REQUIRE(tmp == e.value);
            }
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
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 32; key++) co_await remove(t, key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);
            for (int key = 0; key < 32; key++)
                REQUIRE(co_await find(t, key, nullptr, 0) == false);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reverse deletion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 16; key++) co_await tinsert(t, key, 10*key);
            for (int key = 15; key >= 0; key--) co_await remove(t, key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);
            for (int key = 0; key < 16; key++)
                REQUIRE(co_await find(t, key, nullptr, 0) == false);
            destroy_test_pager(pager);
        }
    }

    SECTION("(page_size=128) reinsertion after complete deletion") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            auto pager = create_test_pager(pfile, 128_u64);
            pager::begin_transaction(pager);
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 8; key++) co_await tinsert(t, key, 10*key);
            for (int key = 0; key < 8; key++) co_await remove(t, key);
            for (int key = 0; key < 8; key++) co_await tinsert(t, key, 100*key);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 8; key++)
                REQUIRE(*co_await tfind<int>(t, key) == 100*key);
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
            header_page = co_await btree::create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 1000; key++) co_await tinsert(t, key, 10*key);
            REQUIRE(co_await size(t) == 1000);

            co_await truncate(t);
            REQUIRE(co_await size(t) == 0);
            co_await pager::commit_transaction(pager);
            destroy_test_pager(pager);
        }

        {
            auto pager = test_pager(pfile);
            BTreePaged t(&pager, header_page);

            REQUIRE(co_await size(t) == 0);
            destroy_test_pager(pager);
        }
    }
}
