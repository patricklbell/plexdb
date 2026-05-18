#include <catch2/catch_test_macros.hpp>
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
import plexdb.coroutine;
import plexdb.os;
import plexdb.pager;
import plexdb.pager.types;
import plexdb.pager.wal;
import plexdb.pager.test_helpers;

using namespace plexdb;

PAGER_TEST_CASE("write and check read back", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 pidx;
    U8 data[] = "some data";
    REQUIRE(sizeof(data) <= page_size);

    {
        auto pager = create_test_pager(f, page_size);
        pager::begin_transaction(pager);
        pidx = co_await new_page(pager);
        U8* buffer = co_await rwpage(pager, pidx);
        os::memory_copy(buffer, data, sizeof(data));
        co_await pager::commit_transaction(pager);
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        REQUIRE(pager.header.page_size == page_size);
        REQUIRE(ArrayView(data) == co_await rpage(pager, pidx));
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("write many pages and check free reclaims space", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;
    auto pager = create_test_pager(f, page_size);

    pager::begin_transaction(pager);
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

    co_await pager::commit_transaction(pager);
    destroy_test_pager(pager);
    REQUIRE(pager.header.page_count == initial_pages);
    REQUIRE(os::file_get_stats(f).byte_count == initial_bytes);
}

PAGER_TEST_CASE("write many pages and check random free does not increase size", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;

    U64 total_pages_after_write;
    U64 total_bytes_after_write;
    std::vector<U64> pages{};
    {
        auto pager = create_test_pager(f, page_size);
        pager::begin_transaction(pager);
        for (int i = 0; i < 1000; i++) {
            pages.push_back(co_await new_page(pager));
        }
        co_await pager::commit_transaction(pager);
        total_pages_after_write = pager.header.page_count;
        total_bytes_after_write = os::file_get_stats(f).byte_count;
        destroy_test_pager(pager);
    }

    {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        auto pager = test_pager(f);
        pager::begin_transaction(pager);
        for (int i = 0; i < 100; i++) {
            co_await delete_page(pager, pages[i]);
        }
        co_await pager::commit_transaction(pager);

        REQUIRE(pager.header.page_count <= total_pages_after_write);
        REQUIRE(os::file_get_stats(f).byte_count <= total_bytes_after_write);
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("pager handles different page sizes and writes", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = GENERATE(32_u64, 128_u64, 4096_u64, 65536_u64);

    auto pager = create_test_pager(f, page_size);
    pager::begin_transaction(pager);

    std::vector<U8> buffer(page_size, 0xAA);
    U64 pidx = co_await new_page(pager);
    os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), buffer.size());
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == co_await rpage(pager, pidx));

    os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), 0);
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == co_await rpage(pager, pidx));

    co_await pager::commit_transaction(pager);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager randomized stress and reuse freed pages", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    auto pager = create_test_pager(f, page_size);
    pager::begin_transaction(pager);

    std::vector<U64> pages;
    for(int i = 0; i < 500; ++i) pages.push_back(co_await new_page(pager));

    std::mt19937 rng(123);
    std::shuffle(pages.begin(), pages.end(), rng);
    for(int i = 0; i < 200; ++i) co_await delete_page(pager, pages[i]);

    for(int i = 0; i < 200; ++i) {
        co_await new_page(pager);
    }
    co_await pager::commit_transaction(pager);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager flush and file size consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));
    os::File walf{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(walf));

    U64 page_size = 128;
    auto pager = create_test_pager(f, walf, page_size);

    pager::begin_transaction(pager);
    U64 pidx = co_await new_page(pager);
    std::vector<U8> buffer(page_size, 0x55);
    os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), buffer.size());
    co_await pager::commit_transaction(pager);
    REQUIRE(os::file_get_stats(f).byte_count >= page_size);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager large-scale allocation", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;
    auto pager = create_test_pager(f, page_size);
    pager::begin_transaction(pager);

    for(int i = 0; i < 10000; ++i) co_await new_page(pager);
    REQUIRE(pager.header.page_count >= 10000);

    co_await pager::commit_transaction(pager);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager alignment and padding", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 40;
    auto pager = create_test_pager(f, page_size);
    pager::begin_transaction(pager);

    co_await new_page(pager);
    REQUIRE(os::file_get_stats(f).byte_count % page_size == 0);

    co_await pager::commit_transaction(pager);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager partial writes do not corrupt", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    U64 pidx;
    {
        auto pager = create_test_pager(f, page_size);
        pager::begin_transaction(pager);
        pidx = co_await new_page(pager);
        std::vector<U8> buffer(page_size, 0xAA);
        os::memory_copy(co_await rwpage(pager, pidx), buffer.data(), page_size / 2);
        co_await pager::commit_transaction(pager);
        destroy_test_pager(pager);
    }

    auto pager = test_pager(f);
    auto readback = co_await rpage(pager, pidx);
    for(U64 i = 0; i < page_size / 2; ++i) REQUIRE(readback[i] == 0xAA);
    destroy_test_pager(pager);
}

PAGER_TEST_CASE("pager repeated reopen and free consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    {
        auto pager = create_test_pager(f, 32);
        pager::begin_transaction(pager);
        U64 pidx = co_await new_page(pager);
        co_await delete_page(pager, pidx);
        co_await pager::commit_transaction(pager);
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        pager::begin_transaction(pager);
        U64 pidx = co_await new_page(pager);
        co_await delete_page(pager, pidx);
        co_await pager::commit_transaction(pager);
        destroy_test_pager(pager);
    }
}

// ============================================================================
// WAL tests
// ============================================================================

PAGER_TEST_CASE("write and read back via WAL-enabled pager", "[plexdb.pager.wal]") {
    os::Handle pid = os::process_get_handle();
    auto db_path = fmt("/tmp/plexdb_wal::basic_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_wal::basic_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 pidx;
    U8 data[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = create_test_pager(db, wal, page_size);
            pager::begin_transaction(p);
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), data, sizeof(data));
            co_await pager::commit_transaction(p);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(data); i++)
                REQUIRE(rb[i] == data[i]);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("committed WAL replays after abrupt process exit", "[plexdb.pager.wal]") {
    os::Handle pid = os::process_get_handle();
    auto db_path = fmt("/tmp/plexdb_wal::crash_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_wal::crash_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 pidx;
    U8 original[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22};
    U8 modified[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};

    {
        os::File db{os::file_open(db_path)};
        {
            auto p = create_test_pager(db, page_size);
            pager::begin_transaction(p);
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await pager::commit_transaction(p);
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

        U8* buf = os::allocate_zero(page_size);
        os::file_read(child_db, Rng1U64{.start=page_size*pidx, .end=page_size*(pidx + 1)}, buf);
        os::memory_copy(buf, modified, sizeof(modified));
        drive_test_pager(wal::append_frame(wal, g_test_sync_file_io_ctx, pidx, buf));
        drive_test_pager(wal::commit(wal, g_test_sync_file_io_ctx));

        os::deallocate(buf);
        os::process_exit(0);
    }
    os::process_wait(child);

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(modified); i++)
                REQUIRE(rb[i] == modified[i]);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("uncommitted frames do not modify the database", "[plexdb.pager.wal]") {
    os::Handle pid = os::process_get_handle();
    auto db_path = fmt("/tmp/plexdb_wal::nocommit_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_wal::nocommit_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 pidx;
    U8 original[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22};
    U8 modified[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};

    {
        os::File db{os::file_open(db_path)}, wal{os::file_open(wal_path)};
        {
            auto p = create_test_pager(db, wal, page_size);
            pager::begin_transaction(p);
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await pager::commit_transaction(p);
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

        U8* buf = os::allocate_zero(page_size);
        os::file_read(child_db, Rng1U64{.start=page_size*pidx, .end=page_size*(pidx + 1)}, buf);
        os::memory_copy(buf, modified, sizeof(modified));
        drive_test_pager(wal::append_frame(wal, g_test_sync_file_io_ctx, pidx, buf));
        // @note no wal::commit — frames are uncommitted

        os::deallocate(buf);
        os::process_exit(0);
    }
    os::process_wait(child);

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(original); i++)
                REQUIRE(rb[i] == original[i]);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("SIGKILL after commit triggers WAL recovery", "[plexdb.pager.wal]") {
    os::Handle pid = os::process_get_handle();
    auto db_path = fmt("/tmp/plexdb_wal::sigkill_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_wal::sigkill_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 pidx;
    U8 original[8] = {0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80};
    U8 modified[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};

    {
        os::File db{os::file_open(db_path)}, wal{os::file_open(wal_path)};
        {
            auto p = create_test_pager(db, wal, page_size);
            pager::begin_transaction(p);
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await pager::commit_transaction(p);
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

        U8* buf = os::allocate_zero(page_size);
        os::file_read(child_db, Rng1U64{.start=page_size*pidx, .end=page_size*(pidx + 1)}, buf);
        os::memory_copy(buf, modified, sizeof(modified));
        drive_test_pager(wal::append_frame(wal, g_test_sync_file_io_ctx, pidx, buf));
        drive_test_pager(wal::commit(wal, g_test_sync_file_io_ctx));

        os::deallocate(buf);

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
            auto p = test_pager(db, wal);
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(modified); i++)
                REQUIRE(rb[i] == modified[i]);
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
    U8 data[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

    {
        auto pager = create_test_pager(f, page_size);
        pager::begin_transaction(pager);
        pidx = co_await new_page(pager);
        os::memory_copy(co_await rwpage(pager, pidx), data, sizeof(data));
        co_await pager::commit_transaction(pager);
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        const U8* rb = co_await rpage(pager, pidx);
        for (U64 i = 0; i < sizeof(data); i++)
            REQUIRE(rb[i] == data[i]);
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("rollback without WAL discards writes and restores page_count", "[plexdb.pager.transaction]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 pidx;
    U8 original[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22};

    {
        auto pager = create_test_pager(f, page_size);
        pager::begin_transaction(pager);
        pidx = co_await new_page(pager);
        os::memory_copy(co_await rwpage(pager, pidx), original, sizeof(original));
        co_await pager::commit_transaction(pager);
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        U64 count_before = pager.header.page_count;

        pager::begin_transaction(pager);
        co_await new_page(pager);
        U8 modified[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
        os::memory_copy(co_await rwpage(pager, pidx), modified, sizeof(modified));
        pager::rollback_transaction(pager);

        REQUIRE(pager.header.page_count == count_before);
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        const U8* rb = co_await rpage(pager, pidx);
        for (U64 i = 0; i < sizeof(original); i++)
            REQUIRE(rb[i] == original[i]);
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("multiple sequential transactions without WAL all persist", "[plexdb.pager.transaction]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 p1, p2;
    U8 data1[8] = {1,2,3,4,5,6,7,8};
    U8 data2[8] = {9,10,11,12,13,14,15,16};

    {
        auto pager = create_test_pager(f, page_size);

        pager::begin_transaction(pager);
        p1 = co_await new_page(pager);
        os::memory_copy(co_await rwpage(pager, p1), data1, sizeof(data1));
        co_await pager::commit_transaction(pager);

        pager::begin_transaction(pager);
        p2 = co_await new_page(pager);
        os::memory_copy(co_await rwpage(pager, p2), data2, sizeof(data2));
        co_await pager::commit_transaction(pager);

        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        const U8* rb1 = co_await rpage(pager, p1);
        for (U64 i = 0; i < sizeof(data1); i++)
            REQUIRE(rb1[i] == data1[i]);
        const U8* rb2 = co_await rpage(pager, p2);
        for (U64 i = 0; i < sizeof(data2); i++)
            REQUIRE(rb2[i] == data2[i]);
        destroy_test_pager(pager);
    }
}

PAGER_TEST_CASE("rollback with WAL discards in-memory writes", "[plexdb.pager.transaction]") {
    os::Handle pid = os::process_get_handle();
    auto db_path  = fmt("/tmp/plexdb_txn::wrollback_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_txn::wrollback_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 pidx;
    U8 original[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22};

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = create_test_pager(db, wal, page_size);
            pager::begin_transaction(p);
            pidx = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, pidx), original, sizeof(original));
            co_await pager::commit_transaction(p);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            U8 modified[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
            pager::begin_transaction(p);
            os::memory_copy(co_await rwpage(p, pidx), modified, sizeof(modified));
            pager::rollback_transaction(p);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = co_await rpage(p, pidx);
            for (U64 i = 0; i < sizeof(original); i++)
                REQUIRE(rb[i] == original[i]);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}

PAGER_TEST_CASE("multiple sequential WAL transactions all persist", "[plexdb.pager.transaction]") {
    os::Handle pid = os::process_get_handle();
    auto db_path  = fmt("/tmp/plexdb_txn::wmulti_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_txn::wmulti_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 p1, p2;
    U8 data1[8] = {1,2,3,4,5,6,7,8};
    U8 data2[8] = {9,10,11,12,13,14,15,16};

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = create_test_pager(db, wal, page_size);

            pager::begin_transaction(p);
            p1 = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, p1), data1, sizeof(data1));
            co_await pager::commit_transaction(p);

            pager::begin_transaction(p);
            p2 = co_await new_page(p);
            os::memory_copy(co_await rwpage(p, p2), data2, sizeof(data2));
            co_await pager::commit_transaction(p);

            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb1 = co_await rpage(p, p1);
            for (U64 i = 0; i < sizeof(data1); i++)
                REQUIRE(rb1[i] == data1[i]);
            const U8* rb2 = co_await rpage(p, p2);
            for (U64 i = 0; i < sizeof(data2); i++)
                REQUIRE(rb2[i] == data2[i]);
            destroy_test_pager(p);
        }
        os::file_close(db);
        os::file_close(wal);
    }

    os::file_delete(db_path);
    os::file_delete(wal_path);
}
