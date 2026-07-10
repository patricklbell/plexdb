#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include <plexdb/test_macros/test_macros.h>

#include <coroutine>
#include <vector>
#include <algorithm>
#include <random>
#include <ranges>
#include <cinttypes>

import plexdb.base;
import plexdb.aio;
import plexdb.coroutine;
import plexdb.os;
import plexdb.pager;
import plexdb.pager.types;
import plexdb.pager.wal;
import plexdb.test.pager_helpers;

using namespace plexdb;

PAGER_TEST_CASE("write and check read back", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 pidx;
    U8  data[] = "some data";
    REQUIRE(sizeof(data) <= page_size);

    {
        auto               pager = create_test_pager(f, page_size);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        pidx       = co_await new_page(pager);
        U8* buffer = co_await rwpage(pager, pidx);
        os::memory_copy(buffer, data, sizeof(data));
        co_await tx.commit();
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        REQUIRE(pager.header.page_size == page_size);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        REQUIRE(ArrayView(data) == co_await rpage(pager, pidx));
        co_await tx.commit();
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("write many pages and check free reclaims space", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64  page_size = 32;
    auto pager     = create_test_pager(f, page_size);

    pager::Transaction tx{&pager};
    co_await tx.begin();
    U64 initial_pages = pager.header.page_count;
    U64 initial_bytes = os::file_get_stats(f).byte_count;

    std::vector<U64> pages{};
    for (int i = 0; i < 1000; i++) {
        pages.push_back(co_await new_page(pager));
    }

    SECTION("forward free") {
        for (const auto& page : std::views::reverse(pages)) {
            co_await delete_page(pager, page);
        }
    }
    SECTION("reverse free") {
        for (const auto& page : pages) {
            co_await delete_page(pager, page);
        }
    }
    SECTION("random free") {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        for (const auto& page : pages) {
            co_await delete_page(pager, page);
        }
    }

    co_await tx.commit();
    destroy_test_pager(pager);
    REQUIRE(pager.header.page_count == initial_pages);
    REQUIRE(os::file_get_stats(f).byte_count == initial_bytes);
}

PAGER_TEST_CASE("write many pages and check random free does not increase size", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;

    U64              total_pages_after_write;
    U64              total_bytes_after_write;
    std::vector<U64> pages{};
    {
        auto               pager = create_test_pager(f, page_size);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        for (int i = 0; i < 1000; i++) {
            pages.push_back(co_await new_page(pager));
        }
        co_await tx.commit();
        total_pages_after_write = pager.header.page_count;
        total_bytes_after_write = os::file_get_stats(f).byte_count;
        destroy_test_pager(pager);
    }

    {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        auto               pager = test_pager(f);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        for (int i = 0; i < 100; i++) {
            co_await delete_page(pager, pages[i]);
        }
        co_await tx.commit();

        REQUIRE(pager.header.page_count <= total_pages_after_write);
        REQUIRE(os::file_get_stats(f).byte_count <= total_bytes_after_write);
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("pager handles different page sizes and writes", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = GENERATE(32_u64, 128_u64, 4096_u64, 65536_u64);

    auto               pager = create_test_pager(f, page_size);
    pager::Transaction tx{&pager};
    co_await tx.begin();

    std::vector<U8> buffer(page_size, 0xAA);
    U64             pidx = co_await new_page(pager);
    os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), buffer.size());
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == co_await rpage(pager, pidx));

    os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), 0);
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == co_await rpage(pager, pidx));

    co_await tx.commit();
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager randomized stress and reuse freed pages", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64                page_size = 64;
    auto               pager     = create_test_pager(f, page_size);
    pager::Transaction tx{&pager};
    co_await tx.begin();

    std::vector<U64> pages;
    for (int i = 0; i < 500; ++i) {
        pages.push_back(co_await new_page(pager));
    }

    std::mt19937 rng(123);
    std::shuffle(pages.begin(), pages.end(), rng);
    for (int i = 0; i < 200; ++i) {
        co_await delete_page(pager, pages[i]);
    }

    for (int i = 0; i < 200; ++i) {
        co_await new_page(pager);
    }
    co_await tx.commit();
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager flush and file size consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));
    os::File walf{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(walf));

    U64  page_size = 128;
    auto pager     = create_test_pager(f, walf, page_size);

    pager::Transaction tx{&pager};
    co_await tx.begin();
    U64             pidx = co_await new_page(pager);
    std::vector<U8> buffer(page_size, 0x55);
    os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), buffer.size());
    co_await tx.commit();
    REQUIRE(os::file_get_stats(f).byte_count >= page_size);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager large-scale allocation", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64                page_size = 32;
    auto               pager     = create_test_pager(f, page_size);
    pager::Transaction tx{&pager};
    co_await tx.begin();

    for (int i = 0; i < 600; ++i) {
        co_await new_page(pager);
    }
    REQUIRE(pager.header.page_count >= 600);

    co_await tx.commit();
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager alignment and padding", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64                page_size = 40;
    auto               pager     = create_test_pager(f, page_size);
    pager::Transaction tx{&pager};
    co_await tx.begin();

    co_await new_page(pager);
    REQUIRE(os::file_get_stats(f).byte_count % page_size == 0);

    co_await tx.commit();
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager partial writes do not corrupt", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    U64 pidx;
    {
        auto               pager = create_test_pager(f, page_size);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        pidx = co_await new_page(pager);
        std::vector<U8> buffer(page_size, 0xAA);
        os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), page_size / 2);
        co_await tx.commit();
        destroy_test_pager(pager);
    }

    auto               pager = test_pager(f);
    pager::Transaction tx{&pager};
    co_await tx.begin();
    auto readback = co_await rpage(pager, pidx);
    for (U64 i = 0; i < page_size / 2; ++i) {
        REQUIRE(readback[i] == 0xAA);
    }
    co_await tx.commit();
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager repeated reopen and free consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    {
        auto               pager = create_test_pager(f, 32);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        U64 pidx = co_await new_page(pager);
        co_await delete_page(pager, pidx);
        co_await tx.commit();
        destroy_test_pager(pager);
    }

    {
        auto               pager = test_pager(f);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        U64 pidx = co_await new_page(pager);
        co_await delete_page(pager, pidx);
        co_await tx.commit();
        destroy_test_pager(pager);
    }
}

// ============================================================================
// WAL tests
// ============================================================================

PAGER_TEST_CASE("write and read back via WAL-enabled pager", "[plexdb.pager.wal]") {
    os::Handle pid      = os::process_get_handle();
    auto       db_path  = fmt("/tmp/plexdb_wal::basic_%" PRIu64 "_db", pid.u64[0]);
    auto       wal_path = fmt("/tmp/plexdb_wal::basic_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path)) {
        os::file_delete(db_path);
    }
    if (os::file_exists(wal_path)) {
        os::file_delete(wal_path);
    }

    U64 page_size = 128;
    U64 pidx;
    U8  data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = create_test_pager(db, wal, page_size);
            pager::Transaction tx{&p};
            co_await tx.begin();
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), data, sizeof(data));
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = test_pager(db, wal);
            pager::Transaction tx{&p};
            co_await tx.begin();
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(data); i++) {
                REQUIRE(rb[i] == data[i]);
            }
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("committed WAL replays after abrupt process exit", "[plexdb.pager.wal]") {
    os::Handle pid      = os::process_get_handle();
    auto       db_path  = fmt("/tmp/plexdb_wal::crash_%" PRIu64 "_db", pid.u64[0]);
    auto       wal_path = fmt("/tmp/plexdb_wal::crash_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path)) {
        os::file_delete(db_path);
    }
    if (os::file_exists(wal_path)) {
        os::file_delete(wal_path);
    }

    U64 page_size = 128;
    U64 pidx;
    U8  original[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    U8  modified[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    {
        os::File db{os::file_open(db_path)};
        {
            auto               p = create_test_pager(db, page_size);
            pager::Transaction tx{&p};
            co_await tx.begin();
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await tx.commit();
            destroy_test_pager(p);
        }
    }

    auto child_opt = os::process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    os::Handle child = *child_opt;
    if (os::is_zero_handle(child)) {
        os::Handle child_db  = os::file_open(db_path);
        os::Handle child_wal = os::file_open(wal_path);

        Wal wal = create_test_wal(child_wal, page_size);

        UniquePtr<U8> buf{os::allocate_zero(page_size)};
        os::file_read(child_db, Rng1U64{.start = page_size * pidx, .end = page_size * (pidx + 1)}, buf.ptr);
        os::memory_copy(buf.ptr, modified, sizeof(modified));
        wal::PageWrite frame_write[1] = {
            {.page_idx = pidx, .data = buf.ptr}
        };
        drive_test_pager(wal::append_frames(wal, g_test_sync_file_io_ctx, TArrayView<const wal::PageWrite>{frame_write, 1}));
        drive_test_pager(wal::commit(wal, g_test_sync_file_io_ctx));

        os::process_exit(0);
    }
    os::process_wait(child);

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = test_pager(db, wal);
            pager::Transaction tx{&p};
            co_await tx.begin();
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(modified); i++) {
                REQUIRE(rb[i] == modified[i]);
            }
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("uncommitted frames do not modify the database", "[plexdb.pager.wal]") {
    os::Handle pid      = os::process_get_handle();
    auto       db_path  = fmt("/tmp/plexdb_wal::nocommit_%" PRIu64 "_db", pid.u64[0]);
    auto       wal_path = fmt("/tmp/plexdb_wal::nocommit_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path)) {
        os::file_delete(db_path);
    }
    if (os::file_exists(wal_path)) {
        os::file_delete(wal_path);
    }

    U64 page_size = 128;
    U64 pidx;
    U8  original[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    U8  modified[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    {
        os::File db{os::file_open(db_path)}, wal{os::file_open(wal_path)};
        {
            auto               p = create_test_pager(db, wal, page_size);
            pager::Transaction tx{&p};
            co_await tx.begin();
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await tx.commit();
            destroy_test_pager(p);
        }
    }

    auto child_opt = os::process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    os::Handle child = *child_opt;
    if (os::is_zero_handle(child)) {
        os::Handle child_db  = os::file_open(db_path);
        os::Handle child_wal = os::file_open(wal_path);

        Wal wal = create_test_wal(child_wal, page_size);

        UniquePtr<U8> buf{os::allocate_zero(page_size)};
        os::file_read(child_db, Rng1U64{.start = page_size * pidx, .end = page_size * (pidx + 1)}, buf.ptr);
        os::memory_copy(buf.ptr, modified, sizeof(modified));
        wal::PageWrite frame_write[1] = {
            {.page_idx = pidx, .data = buf.ptr}
        };
        drive_test_pager(wal::append_frames(wal, g_test_sync_file_io_ctx, TArrayView<const wal::PageWrite>{frame_write, 1}));
        // @note no wal::commit — frames are uncommitted

        os::process_exit(0);
    }
    os::process_wait(child);

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = test_pager(db, wal);
            pager::Transaction tx{&p};
            co_await tx.begin();
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(original); i++) {
                REQUIRE(rb[i] == original[i]);
            }
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("SIGKILL after commit triggers WAL recovery", "[plexdb.pager.wal]") {
    os::Handle pid      = os::process_get_handle();
    auto       db_path  = fmt("/tmp/plexdb_wal::sigkill_%" PRIu64 "_db", pid.u64[0]);
    auto       wal_path = fmt("/tmp/plexdb_wal::sigkill_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path)) {
        os::file_delete(db_path);
    }
    if (os::file_exists(wal_path)) {
        os::file_delete(wal_path);
    }

    U64 page_size = 128;
    U64 pidx;
    U8  original[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    U8  modified[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    {
        os::File db{os::file_open(db_path)}, wal{os::file_open(wal_path)};
        {
            auto               p = create_test_pager(db, wal, page_size);
            pager::Transaction tx{&p};
            co_await tx.begin();
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await tx.commit();
            destroy_test_pager(p);
        }
    }

    os::Notifier notifier{};

    auto child_opt = os::process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    os::Handle child = *child_opt;
    if (os::is_zero_handle(child)) {
        os::Handle child_db  = os::file_open(db_path);
        os::Handle child_wal = os::file_open(wal_path);

        Wal wal = create_test_wal(child_wal, page_size);

        UniquePtr<U8> buf{os::allocate_zero(page_size)};
        os::file_read(child_db, Rng1U64{.start = page_size * pidx, .end = page_size * (pidx + 1)}, buf.ptr);
        os::memory_copy(buf.ptr, modified, sizeof(modified));
        wal::PageWrite frame_write[1] = {
            {.page_idx = pidx, .data = buf.ptr}
        };
        drive_test_pager(wal::append_frames(wal, g_test_sync_file_io_ctx, TArrayView<const wal::PageWrite>{frame_write, 1}));
        drive_test_pager(wal::commit(wal, g_test_sync_file_io_ctx));

        os::signal_notify_safe(notifier);
        os::process_pause();
    }

    U8 byte = 0;
    os::stream_read(notifier.read, &byte, 1);
    os::signal_send_kill(child);
    os::process_wait(child);

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = test_pager(db, wal);
            pager::Transaction tx{&p};
            co_await tx.begin();
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(modified); i++) {
                REQUIRE(rb[i] == modified[i]);
            }
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

// ============================================================================
// Transactions
// ============================================================================

PAGER_TEST_CASE("begin and commit without WAL writes data to disk", "[plexdb.pager.transaction]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 pidx;
    U8  data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    {
        auto               pager = create_test_pager(f, page_size);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        pidx = co_await new_page(pager);
        os::memory_copy(co_await rwpage(pager, pidx), data, sizeof(data));
        co_await tx.commit();
        destroy_test_pager(pager);
    }

    {
        auto               pager = test_pager(f);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        const U8* rb = co_await rpage(pager, pidx);
        for (U64 i = 0; i < sizeof(data); i++) {
            REQUIRE(rb[i] == data[i]);
        }
        co_await tx.commit();
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("rollback without WAL discards writes and restores page_count", "[plexdb.pager.transaction]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 pidx;
    U8  original[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};

    {
        auto               pager = create_test_pager(f, page_size);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        pidx = co_await new_page(pager);
        os::memory_copy(co_await rwpage(pager, pidx), original, sizeof(original));
        co_await tx.commit();
        destroy_test_pager(pager);
    }

    {
        auto pager        = test_pager(f);
        U64  count_before = pager.header.page_count;

        pager::Transaction tx{&pager};
        co_await tx.begin();
        co_await new_page(pager);
        U8 modified[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        os::memory_copy(co_await rwpage(pager, pidx), modified, sizeof(modified));
        tx.rollback();

        REQUIRE(pager.header.page_count == count_before);
        destroy_test_pager(pager);
    }

    {
        auto               pager = test_pager(f);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        const U8* rb = co_await rpage(pager, pidx);
        for (U64 i = 0; i < sizeof(original); i++) {
            REQUIRE(rb[i] == original[i]);
        }
        co_await tx.commit();
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("multiple sequential transactions without WAL all persist", "[plexdb.pager.transaction]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 p1, p2;
    U8  data1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    U8  data2[8] = {9, 10, 11, 12, 13, 14, 15, 16};

    {
        auto pager = create_test_pager(f, page_size);

        {
            pager::Transaction tx{&pager};
            co_await tx.begin();
            p1 = co_await new_page(pager);
            os::memory_copy(co_await rwpage(pager, p1), data1, sizeof(data1));
            co_await tx.commit();
        }

        {
            pager::Transaction tx{&pager};
            co_await tx.begin();
            p2 = co_await new_page(pager);
            os::memory_copy(co_await rwpage(pager, p2), data2, sizeof(data2));
            co_await tx.commit();
        }

        destroy_test_pager(pager);
    }

    {
        auto               pager = test_pager(f);
        pager::Transaction tx{&pager};
        co_await tx.begin();
        const U8* rb1 = co_await rpage(pager, p1);
        for (U64 i = 0; i < sizeof(data1); i++) {
            REQUIRE(rb1[i] == data1[i]);
        }
        const U8* rb2 = co_await rpage(pager, p2);
        for (U64 i = 0; i < sizeof(data2); i++) {
            REQUIRE(rb2[i] == data2[i]);
        }
        co_await tx.commit();
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("rollback with WAL discards in-memory writes", "[plexdb.pager.transaction]") {
    os::Handle pid      = os::process_get_handle();
    auto       db_path  = fmt("/tmp/plexdb_txn::wrollback_%" PRIu64 "_db", pid.u64[0]);
    auto       wal_path = fmt("/tmp/plexdb_txn::wrollback_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path)) {
        os::file_delete(db_path);
    }
    if (os::file_exists(wal_path)) {
        os::file_delete(wal_path);
    }

    U64 page_size = 128;
    U64 pidx;
    U8  original[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = create_test_pager(db, wal, page_size);
            pager::Transaction tx{&p};
            co_await tx.begin();
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p           = test_pager(db, wal);
            U8                 modified[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
            pager::Transaction tx{&p};
            co_await tx.begin();
            os::memory_copy(co_await rwpage(p, pidx), modified, sizeof(modified));
            tx.rollback();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = test_pager(db, wal);
            pager::Transaction tx{&p};
            co_await tx.begin();
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(original); i++) {
                REQUIRE(rb[i] == original[i]);
            }
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("multiple sequential WAL transactions all persist", "[plexdb.pager.transaction]") {
    os::Handle pid      = os::process_get_handle();
    auto       db_path  = fmt("/tmp/plexdb_txn::wmulti_%" PRIu64 "_db", pid.u64[0]);
    auto       wal_path = fmt("/tmp/plexdb_txn::wmulti_%" PRIu64 "_wal", pid.u64[0]);

    if (os::file_exists(db_path)) {
        os::file_delete(db_path);
    }
    if (os::file_exists(wal_path)) {
        os::file_delete(wal_path);
    }

    U64 page_size = 128;
    U64 p1, p2;
    U8  data1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    U8  data2[8] = {9, 10, 11, 12, 13, 14, 15, 16};

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = create_test_pager(db, wal, page_size);

            {
                pager::Transaction tx{&p};
                co_await tx.begin();
                p1 = co_await new_page(p);
                os::memory_copy(co_await rwpage(p, p1), data1, sizeof(data1));
                co_await tx.commit();
            }

            {
                pager::Transaction tx{&p};
                co_await tx.begin();
                p2 = co_await new_page(p);
                os::memory_copy(co_await rwpage(p, p2), data2, sizeof(data2));
                co_await tx.commit();
            }

            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto               p = test_pager(db, wal);
            pager::Transaction tx{&p};
            co_await tx.begin();
            const U8* rb1 = co_await rpage(p, p1);
            for (U64 i = 0; i < sizeof(data1); i++) {
                REQUIRE(rb1[i] == data1[i]);
            }
            const U8* rb2 = co_await rpage(p, p2);
            for (U64 i = 0; i < sizeof(data2); i++) {
                REQUIRE(rb2[i] == data2[i]);
            }
            co_await tx.commit();
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

// ============================================================================
// Concurrent transaction tests (Phase 0: awaitable queue)
// ============================================================================

PAGER_TEST_CASE("concurrent begin_transaction serializes second writer via queue", "[plexdb.pager.transaction]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64  page_size = 128;
    auto pager     = create_test_pager(f, page_size);

    // yield_point captures task1's suspension handle so we can pause mid-transaction
    std::coroutine_handle<> yield_point{};

    // Task1: begin transaction, allocate a page, then pause (simulating a long operation),
    // then commit.
    // @note task1_fn must outlive task1: an eager coroutine's frame keeps
    // referencing its [&]-captured closure on every resume, so an
    // immediately-invoked lambda temporary would dangle after the first one.
    auto task1_fn = [&]() -> coroutine::Task<U64> {
        pager::Transaction tx{&pager};
        co_await tx.begin();
        U64 p1 = co_await pager::new_page(pager);
        // Pause mid-transaction so task2 can attempt to begin_transaction
        co_await coroutine::Awaitable{
            [&](std::coroutine_handle<> h) { yield_point = h; },
            [] {
            }
        };
        co_await tx.commit();
        co_return p1;
    };
    auto task1 = task1_fn();

    // Task2: begin transaction (should queue because task1 holds it), allocate a page, commit.
    U64  p2       = 0;
    auto task2_fn = [&]() -> coroutine::Task<> {
        pager::Transaction tx{&pager};
        co_await tx.begin();
        p2 = co_await pager::new_page(pager);
        co_await tx.commit();
    };
    auto task2 = task2_fn();

    // Drive task1 until it pauses at the artificial yield inside the transaction
    task1.resume();
    REQUIRE(!task1.done());
    REQUIRE(pager.transaction_active);

    // Drive task2: it tries to begin_transaction, finds it active, and queues
    task2.resume();
    REQUIRE(!task2.done());
    REQUIRE(pager.tx_waiters.length == 1);

    // Resume task1 via yield_point — task1 commits which wakes task2;
    // task2 runs its full body and commits, then task1 finishes
    yield_point.resume();

    REQUIRE(task1.done());
    REQUIRE(task2.done());
    REQUIRE(p2 > 0);
    REQUIRE(!pager.transaction_active);
    REQUIRE(pager.tx_waiters.length == 0);

    destroy_test_pager(pager);
    co_return;
}

// ============================================================================
// WAL frame coalescing (append_frames)
// ============================================================================

PAGER_TEST_CASE("wal::append_frames coalesces a multi-page batch and round-trips through read_frame", "[plexdb.pager.wal]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    Wal wal       = create_test_wal(f, page_size);

    aio::FileIOContext ctx = aio::create_sync_file_io_context();

    U8 page_a[64];
    U8 page_b[64];
    U8 page_c[64];
    U8 page_d[64];
    os::memory_set(page_a, 0xAA, page_size);
    os::memory_set(page_b, 0xBB, page_size);
    os::memory_set(page_c, 0xCC, page_size);
    os::memory_set(page_d, 0xDD, page_size);

    pager::Header header{
        .magic      = pager::HEADER_MAGIC,
        .version    = pager::HEADER_CURRENT_VERSION,
        .page_size  = page_size,
        .page_count = 5,
        .root_page  = 3,
    };

    // Header-sentinel frame in the middle of the batch (not last): its
    // shorter-than-stride payload lands inside a merged chunk, not at the end.
    wal::PageWrite writes[5] = {
        {      .page_idx = 1,                               .data = page_a},
        {      .page_idx = 2,                               .data = page_b},
        {.page_idx = MAX_U64, .data = reinterpret_cast<const U8*>(&header)},
        {      .page_idx = 3,                               .data = page_c},
        {      .page_idx = 4,                               .data = page_d},
    };

    co_await wal::append_frames(wal, ctx, TArrayView<const wal::PageWrite>{writes, 5});

    REQUIRE(wal.header.frame_count == 5);

    // @note frame indices are positional: this WAL was fresh, so frame_idx == write_idx.
    for (U64 write_idx = 0; write_idx < 5; write_idx++) {
        wal::Frame frame{};
        U8         buf[64] = {};
        co_await wal::read_frame(wal, ctx, write_idx, frame, buf);
        REQUIRE(frame.page_idx == writes[write_idx].page_idx);
        U64 data_size = writes[write_idx].page_idx == MAX_U64 ? sizeof(pager::Header) : page_size;
        REQUIRE(os::memory_compare(buf, writes[write_idx].data, data_size) == 0);
    }
}

TEST_CASE("benchmark: wal::append_frames coalesced vs per-frame writes", "[plexdb.pager.wal][!benchmark]") {
    constexpr U64 page_size = 4096;

    for (U64 n : {1_u64, 5_u64, 20_u64, 100_u64}) {
        DYNAMIC_SECTION("N=" << n) {
            os::File f{os::file_tmp()};
            REQUIRE(!os::is_zero_handle(f));
            Wal                wal = create_test_wal(f, page_size);
            aio::FileIOContext ctx = aio::create_sync_file_io_context();

            std::vector<std::vector<U8>> page_storage(n, std::vector<U8>(page_size, 0xAB));
            std::vector<wal::PageWrite>  writes(n);
            for (U64 i = 0; i < n; i++) {
                writes[i] = wal::PageWrite{.page_idx = i + 1, .data = page_storage[i].data()};
            }

            BENCHMARK_ADVANCED("N separate single-frame append_frames calls")(Catch::Benchmark::Chronometer meter) {
                meter.measure([&](int) {
                    for (U64 write_idx = 0; write_idx < n; write_idx++) {
                        wal::PageWrite single[1] = {writes[write_idx]};
                        drive_test_pager(wal::append_frames(wal, ctx, TArrayView<const wal::PageWrite>{single, 1}));
                    }
                });
            };
            BENCHMARK_ADVANCED("one coalesced append_frames(N) call")(Catch::Benchmark::Chronometer meter) {
                meter.measure([&](int) {
                    drive_test_pager(wal::append_frames(wal, ctx, TArrayView<const wal::PageWrite>{writes.data(), n}));
                });
            };
        }
    }
}

TEST_CASE("benchmark: checkpoint serial vs concurrent page writes", "[plexdb.pager.transaction][!benchmark]") {
    constexpr U64 page_size = 4096;

    for (U64 n : {1_u64, 5_u64, 20_u64, 100_u64}) {
        DYNAMIC_SECTION("N=" << n) {
            os::File db{os::file_tmp()};
            os::File wal_f{os::file_tmp()};
            REQUIRE(!os::is_zero_handle(db));
            REQUIRE(!os::is_zero_handle(wal_f));

            auto pager              = create_test_pager(db, wal_f, page_size);
            pager.header.page_count = n + 1;

            std::vector<std::vector<U8>> page_storage(n, std::vector<U8>(page_size, 0xCD));
            std::vector<wal::PageWrite>  writes(n);
            for (U64 i = 0; i < n; i++) {
                writes[i] = wal::PageWrite{.page_idx = i + 1, .data = page_storage[i].data()};
            }

            auto reload_wal_index = [&]() {
                clear(pager.wal.wal_index);
                U64 base_frame_idx = pager.wal.header.frame_count;
                drive_test_pager(wal::append_frames(pager.wal, g_test_sync_file_io_ctx, TArrayView<const wal::PageWrite>{writes.data(), n}));
                for (U64 write_idx = 0; write_idx < n; write_idx++) {
                    insert(pager.wal.wal_index, writes[write_idx].page_idx, base_frame_idx + write_idx);
                }
            };

            // Sequential one-write-at-a-time checkpoint, for comparison against
            // pager::checkpoint below — includes the same finalize steps
            // (resize/sync/wal::reset) so the comparison isn't skewed by omitting
            // an fsync.
            auto checkpoint_serial = [&]() -> coroutine::Task<> {
                aio::FileIOContext& ctx = *pager.file_io_ctx;
                for (auto& pair : pager.wal.wal_index) {
                    U64 db_start = pager.base_offset + page_size * pair.first;
                    co_await aio::file_write(ctx, pager.file, Rng1U64{.start = db_start, .end = db_start + page_size}, writes[pair.first - 1].data);
                }
                os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * page_size);
                co_await aio::file_sync(ctx, pager.file);
                co_await wal::reset(pager.wal, ctx);
                clear(pager.wal.wal_index);
            };

            BENCHMARK_ADVANCED("serial one-write-at-a-time checkpoint")(Catch::Benchmark::Chronometer meter) {
                meter.measure([&](int) {
                    reload_wal_index();
                    drive_test_pager(checkpoint_serial());
                });
            };
            BENCHMARK_ADVANCED("pager::checkpoint (concurrent)")(Catch::Benchmark::Chronometer meter) {
                meter.measure([&](int) {
                    reload_wal_index();
                    drive_test_pager(pager::checkpoint(pager));
                });
            };

            destroy_test_pager(pager);
        }
    }
}

// ============================================================================
// checkpoint concurrency
// ============================================================================

namespace {
    // Every read/write suspends and records itself here instead of completing
    // immediately, so a test can drive completions in a chosen order rather
    // than the sync backend's incidental FIFO ordering.
    struct ManualOp {
        os::Handle              file{};
        Rng1U64                 rng{};
        void*                   read_dst  = nullptr;
        const void*             write_src = nullptr;
        std::coroutine_handle<> handle{};
    };

    struct ManualIOState {
        ManualOp ops[32]{};
        U64      op_count = 0;
    };

    aio::FileIOContext make_manual_io_context(ManualIOState* state) {
        return aio::FileIOContext{
            aio::FileReadFunctor{[state](os::Handle f, Rng1U64 rng, void* out) -> coroutine::Task<> {
                assert_true(state->op_count < 32, "manual io: too many concurrent ops");
                ManualOp* op = &state->ops[state->op_count++];
                op->file     = f;
                op->rng      = rng;
                op->read_dst = out;
                co_await coroutine::Awaitable{
                    [op](std::coroutine_handle<> h) { op->handle = h; },
                    [op]() {
                        os::file_read(op->file, op->rng, op->read_dst);
                    }
                };
            }},
            aio::FileWriteFunctor{[state](os::Handle f, Rng1U64 rng, const void* in) -> coroutine::Task<> {
                assert_true(state->op_count < 32, "manual io: too many concurrent ops");
                ManualOp* op  = &state->ops[state->op_count++];
                op->file      = f;
                op->rng       = rng;
                op->write_src = in;
                co_await coroutine::Awaitable{
                    [op](std::coroutine_handle<> h) { op->handle = h; },
                    [op]() {
                        os::file_write(op->file, op->rng, op->write_src);
                    }
                };
            }},
            aio::FileSyncFunctor{[](os::Handle f) -> coroutine::Task<> {
                os::file_sync(f);
                co_return;
            }}
        };
    }
}

PAGER_TEST_CASE("checkpoint writes correct per-page content when completions arrive out of order", "[plexdb.pager.transaction]") {
    os::File db{os::file_tmp()};
    os::File wal_f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(db));
    REQUIRE(!os::is_zero_handle(wal_f));

    U64  page_size          = 64;
    auto pager              = create_test_pager(db, wal_f, page_size);
    pager.header.page_count = 4; // pages 1..3 valid; bypasses new_page's bitmap bookkeeping

    U8 page1[64];
    U8 page2[64];
    U8 page3[64];
    os::memory_set(page1, 0x11, page_size);
    os::memory_set(page2, 0x22, page_size);
    os::memory_set(page3, 0x33, page_size);

    wal::PageWrite writes[3] = {
        {.page_idx = 1, .data = page1},
        {.page_idx = 2, .data = page2},
        {.page_idx = 3, .data = page3},
    };
    // @note fresh WAL, so frame_idx == write_idx.
    co_await wal::append_frames(pager.wal, g_test_sync_file_io_ctx, TArrayView<const wal::PageWrite>{writes, 3});
    for (U64 write_idx = 0; write_idx < 3; write_idx++) {
        insert(pager.wal.wal_index, writes[write_idx].page_idx, write_idx);
    }
    // @note pager.page_cache stays empty, so all three entries are uncached and
    // checkpoint must read them back from the WAL under concurrency.

    ManualIOState      io_state{};
    aio::FileIOContext manual_ctx = make_manual_io_context(&io_state);
    pager.file_io_ctx             = &manual_ctx;

    auto checkpoint_task = pager::checkpoint(pager);
    checkpoint_task.resume();

    while (!checkpoint_task.done()) {
        U64 candidates[32];
        U64 candidate_count = 0;
        for (U64 i = 0; i < io_state.op_count; i++) {
            if (io_state.ops[i].handle) {
                candidates[candidate_count++] = i;
            }
        }
        REQUIRE(candidate_count > 0);
        // Pick from the middle of the pending set rather than FIFO/LIFO order,
        // so different iterations' reads and writes genuinely interleave
        // instead of one iteration draining to completion before the next starts.
        U64  pick                 = candidates[candidate_count / 2];
        auto h                    = io_state.ops[pick].handle;
        io_state.ops[pick].handle = {};
        h.resume();
    }

    pager.file_io_ctx = &g_test_sync_file_io_ctx;

    for (U64 i = 0; i < 3; i++) {
        U8  readback[64] = {};
        U64 offset       = page_size * writes[i].page_idx;
        os::file_read(db, Rng1U64{.start = offset, .end = offset + page_size}, readback);
        REQUIRE(os::memory_compare(readback, writes[i].data, page_size) == 0);
    }

    destroy_test_pager(pager);
    co_return;
}
