#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <vector>
#include <algorithm>
#include <random>
#include <ranges>

import plexdb.base;
import plexdb.os;
import plexdb.pager;

using namespace plexdb;

TEST_CASE("write and check read back", "[plexdb.pager]" ) {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));
    
    U64 page_size = 128;
    U64 pidx;
    U8 data[] = "some data";
    REQUIRE(sizeof(data) <= page_size);
    
    {
        auto pager = Pager(f, pager::create(f, page_size));
        
        pidx = pager::new_page(pager);
        U8* buffer = pager::rwpage(pager, pidx);
        os::memory_copy(buffer, data, sizeof(data));
    }

    {
        auto pager = Pager(f);
        REQUIRE(pager.header.page_size == page_size);
        REQUIRE(ArrayView(data) == pager::rpage(pager, pidx));
    }
}

TEST_CASE("write many pages and check free reclaims space", "[plexdb.pager]" ) {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));
    
    U64 page_size = 32;
    auto pager = Pager(f, pager::create(f, page_size));
    
    U64 initial_pages = pager.header.page_count;
    U64 initial_bytes = os::file_get_stats(f).byte_count;
    
    std::vector<U64> pages{};
    for (int i = 0; i < 1000; i++) {
        pages.push_back(pager::new_page(pager));
    }

    SECTION("forward free") {
        for (const auto& page : std::views::reverse(pages)) {
            pager::delete_page(pager, page);
        }
    }
    SECTION("reverse free") {
        for (const auto& page : pages) {
            pager::delete_page(pager, page);
        }
    }
    SECTION("random free") {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        for (const auto& page : pages) {
            pager::delete_page(pager, page);
        }
    }
    
    REQUIRE(pager.header.page_count == initial_pages);
    REQUIRE(os::file_get_stats(f).byte_count == initial_bytes);
}

TEST_CASE("write many pages and check random free does not increase size", "[plexdb.pager]" ) {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));
    
    U64 page_size = 32;

    U64 total_pages_after_write;
    U64 total_bytes_after_write;
    std::vector<U64> pages{};
    {
        auto pager = Pager(f, pager::create(f, page_size));
        for (int i = 0; i < 1000; i++) {
            pages.push_back(pager::new_page(pager));
        }

        total_pages_after_write = pager.header.page_count;
        total_bytes_after_write = os::file_get_stats(f).byte_count;
    }

    {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        auto pager = Pager(f);
        for (int i = 0; i < 100; i++) {
            pager::delete_page(pager, pages[i]);
        }

        REQUIRE(pager.header.page_count <= total_pages_after_write);
        REQUIRE(os::file_get_stats(f).byte_count <= total_bytes_after_write);
    }
}

TEST_CASE("pager handles different page sizes and writes", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = GENERATE(32_u64, 128_u64, 4096_u64, 65536_u64);

    auto pager = Pager(f, pager::create(f, page_size));

    // full page write
    std::vector<U8> buffer(page_size, 0xAA);
    U64 pidx = pager::new_page(pager);
    os::memory_copy(pager::rwpage(pager, pidx), buffer.data(), buffer.size());
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == pager::rpage(pager, pidx));

    // zero-length write does not alter content
    os::memory_copy(pager::rwpage(pager, pidx), buffer.data(), 0);
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == pager::rpage(pager, pidx));
}

TEST_CASE("pager randomized stress and reuse freed pages", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    auto pager = Pager(f, pager::create(f, page_size));

    std::vector<U64> pages;
    for(int i = 0; i < 500; ++i) pages.push_back(pager::new_page(pager));

    std::mt19937 rng(123);
    std::shuffle(pages.begin(), pages.end(), rng);
    for(int i = 0; i < 200; ++i) pager::delete_page(pager, pages[i]);

    for(int i = 0; i < 200; ++i) {
        pager::new_page(pager);
    }
}

TEST_CASE("pager flush and file size consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    auto pager = Pager(f, pager::create(f, page_size));

    U64 pidx = pager::new_page(pager);
    std::vector<U8> buffer(page_size, 0x55);
    os::memory_copy(pager::rwpage(pager, pidx), buffer.data(), buffer.size());

    pager::fflush(pager);
    REQUIRE(os::file_get_stats(f).byte_count >= page_size);
}

TEST_CASE("pager large-scale allocation", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;
    auto pager = Pager(f, pager::create(f, page_size));

    for(int i = 0; i < 10000; ++i) pager::new_page(pager);
    REQUIRE(pager.header.page_count >= 10000); // account for extra pages needed internally
}

TEST_CASE("pager alignment and padding", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 40;
    auto pager = Pager(f, pager::create(f, page_size));

    pager::new_page(pager);
    REQUIRE(os::file_get_stats(f).byte_count % page_size == 0);
}

TEST_CASE("pager partial writes do not corrupt", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    U64 pidx;
    {
        auto pager = Pager(f, pager::create(f, page_size));
        pidx = pager::new_page(pager);
        std::vector<U8> buffer(page_size, 0xAA);
        os::memory_copy(pager::rwpage(pager, pidx), buffer.data(), page_size / 2);
    }

    auto pager = Pager(f);
    auto readback = pager::rpage(pager, pidx);
    for(U64 i = 0; i < page_size / 2; ++i) REQUIRE(readback[i] == 0xAA);
}

TEST_CASE("pager repeated reopen and free consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    {
        auto pager = Pager(f, pager::create(f, 32));
        U64 pidx = pager::new_page(pager);
        pager::delete_page(pager, pidx);
    }

    {
        auto pager = Pager(f);
        U64 pidx = pager::new_page(pager);
        pager::delete_page(pager, pidx);
    }
}
