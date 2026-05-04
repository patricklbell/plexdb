#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <algorithm>
#include <vector>

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.btree;
import plexdb.btree.print;
import plexdb.pager;

using namespace plexdb;
using namespace plexdb::btree;

TEST_CASE("insert", "[plexdb.btree.paged]") {
    SECTION("(page_size=128) consecutive insertion") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++) {
                tinsert(t, key, 10*key);

                for (int i = 0; i <= key; i++)
                    REQUIRE(*tfind<int>(t, i) == 10*i);
            }
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++)
                REQUIRE(*tfind<int>(t, key) == 10*key);
        }
    }

    SECTION("(page_size=128) duplicate key insertion overwrites value") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 5; key++) tinsert(t, key, 10*key);
            for (int key = 0; key < 5; key++) tinsert(t, key, 100*key);
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 5; key++)
                REQUIRE(*tfind<int>(t, key) == 100*key);
        }
    }

    SECTION("(page_size=128) reverse insertion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 31; key >= 0; key--) tinsert(t, key, 10*key);
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++)
                REQUIRE(*tfind<int>(t, key) == 10*key);
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
            Pager pager(pfile, pager::create(pfile, 256_u64));
            header_page = create_paged(pager, 9);
            BTreePaged t(&pager, header_page);

            U64 max_value_length = 0;
            for (auto& e : entries) {
                insert(t, e.key, e.value.data(), e.value.size());
                max_value_length = std::max(max_value_length, e.value.size());
            }
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);

            std::vector<U8> tmp;
            tmp.resize(5);
            for (auto& e : entries) {
                tmp.resize(e.value.size());
                REQUIRE(find(t, e.key, tmp.data(), tmp.size()));
                REQUIRE(tmp == e.value);
            }
        }
    }
}

TEST_CASE("remove", "[plexdb.btree.paged]") {
    SECTION("(page_size=128) consecutive deletion in order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 32; key++) tinsert(t, key, 10*key);
            for (int key = 0; key < 32; key++) remove(t, key);
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);
            for (int key = 0; key < 32; key++) 
                REQUIRE(find(t, key, nullptr, 0) == false);
        }
    }

    SECTION("(page_size=128) reverse deletion order") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 16; key++) tinsert(t, key, 10*key);
            for (int key = 15; key >= 0; key--) remove(t, key);
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);
            for (int key = 0; key < 16; key++) 
                REQUIRE(find(t, key, nullptr, 0) == false);
        }
    }

    SECTION("(page_size=128) reinsertion after complete deletion") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 8; key++) tinsert(t, key, 10*key);
            for (int key = 0; key < 8; key++) remove(t, key);
            for (int key = 0; key < 8; key++) tinsert(t, key, 100*key);
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 8; key++) 
                REQUIRE(*tfind<int>(t, key) == 100*key);
        }
    }
}


TEST_CASE("truncate", "[plexdb.btree.paged]" ) {
    SECTION("(page_size=128) insert then truncate and check pages are freed") {
        os::File pfile(os::file_tmp());
        U64 header_page;

        {
            Pager pager(pfile, pager::create(pfile, 128_u64));
            header_page = create_paged(pager, sizeof(int));
            BTreePaged t(&pager, header_page);

            for (int key = 0; key < 1000; key++) tinsert(t, key, 10*key);
            REQUIRE(size(t) == 1000);

            truncate(t);
            REQUIRE(size(t) == 0);
            // @note we can't assert page count because new root pages may be allocated near the end, stopping trimming
            // @todo introduce tracking of free pages for testing purposes
            // @todo investigate if we should reallocate the root page
        }

        {
            Pager pager(pfile);
            BTreePaged t(&pager, header_page);

            REQUIRE(size(t) == 0);
        }
    }
}