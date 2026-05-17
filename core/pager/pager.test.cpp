#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <vector>
#include <algorithm>
#include <random>
#include <ranges>
#include <cinttypes>

import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.pager.types;
import plexdb.pager.wal;
import plexdb.pager.test_helpers;

using namespace plexdb;

TEST_CASE("write and check read back", "[plexdb.pager]" ) {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    U64 pidx;
    U8 data[] = "some data";
    REQUIRE(sizeof(data) <= page_size);

    {
        auto pager = create_test_pager(f, page_size);

        pidx = drive_test_pager(new_page(pager));
        U8* buffer = drive_test_pager(rwpage(pager, pidx));
        os::memory_copy(buffer, data, sizeof(data));
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        REQUIRE(pager.header.page_size == page_size);
        REQUIRE(ArrayView(data) == drive_test_pager(rpage(pager, pidx)));
        destroy_test_pager(pager);
    }
}

TEST_CASE("write many pages and check free reclaims space", "[plexdb.pager]" ) {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;
    auto pager = create_test_pager(f, page_size);

    U64 initial_pages = pager.header.page_count;
    U64 initial_bytes = os::file_get_stats(f).byte_count;

    std::vector<U64> pages{};
    for (int i = 0; i < 1000; i++) {
        pages.push_back(drive_test_pager(new_page(pager)));
    }

    SECTION("forward free") {
        for (const auto& page : std::views::reverse(pages)) {
            drive_test_pager(delete_page(pager, page));
        }
    }
    SECTION("reverse free") {
        for (const auto& page : pages) {
            drive_test_pager(delete_page(pager, page));
        }
    }
    SECTION("random free") {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        for (const auto& page : pages) {
            drive_test_pager(delete_page(pager, page));
        }
    }

    destroy_test_pager(pager);
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
        auto pager = create_test_pager(f, page_size);
        for (int i = 0; i < 1000; i++) {
            pages.push_back(drive_test_pager(new_page(pager)));
        }

        total_pages_after_write = pager.header.page_count;
        total_bytes_after_write = os::file_get_stats(f).byte_count;
        destroy_test_pager(pager);
    }

    {
        std::mt19937 rng(GENERATE(take(1, random(0_u32, MAX_U32))));
        std::shuffle(pages.begin(), pages.end(), rng);

        auto pager = test_pager(f);
        for (int i = 0; i < 100; i++) {
            drive_test_pager(delete_page(pager, pages[i]));
        }

        REQUIRE(pager.header.page_count <= total_pages_after_write);
        REQUIRE(os::file_get_stats(f).byte_count <= total_bytes_after_write);
        destroy_test_pager(pager);
    }
}

TEST_CASE("pager handles different page sizes and writes", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = GENERATE(32_u64, 128_u64, 4096_u64, 65536_u64);

    auto pager = create_test_pager(f, page_size);

    // full page write
    std::vector<U8> buffer(page_size, 0xAA);
    U64 pidx = drive_test_pager(new_page(pager));
    os::memory_copy(drive_test_pager(rwpage(pager, pidx)), buffer.data(), buffer.size());
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == drive_test_pager(rpage(pager, pidx)));

    // zero-length write does not alter content
    os::memory_copy(drive_test_pager(rwpage(pager, pidx)), buffer.data(), 0);
    REQUIRE(ArrayView(buffer.data(), sizeof(U8), buffer.size()) == drive_test_pager(rpage(pager, pidx)));
    destroy_test_pager(pager);
}

TEST_CASE("pager randomized stress and reuse freed pages", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    auto pager = create_test_pager(f, page_size);

    std::vector<U64> pages;
    for(int i = 0; i < 500; ++i) pages.push_back(drive_test_pager(new_page(pager)));

    std::mt19937 rng(123);
    std::shuffle(pages.begin(), pages.end(), rng);
    for(int i = 0; i < 200; ++i) drive_test_pager(delete_page(pager, pages[i]));

    for(int i = 0; i < 200; ++i) {
        drive_test_pager(new_page(pager));
    }
    destroy_test_pager(pager);
}

TEST_CASE("pager flush and file size consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 128;
    auto pager = create_test_pager(f, page_size);

    U64 pidx = drive_test_pager(new_page(pager));
    std::vector<U8> buffer(page_size, 0x55);
    os::memory_copy(drive_test_pager(rwpage(pager, pidx)), buffer.data(), buffer.size());

    drive_test_pager(fflush(pager));
    REQUIRE(os::file_get_stats(f).byte_count >= page_size);
    destroy_test_pager(pager);
}

TEST_CASE("pager large-scale allocation", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 32;
    auto pager = create_test_pager(f, page_size);

    for(int i = 0; i < 10000; ++i) drive_test_pager(new_page(pager));
    REQUIRE(pager.header.page_count >= 10000); // account for extra pages needed internally
    destroy_test_pager(pager);
}

TEST_CASE("pager alignment and padding", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 40;
    auto pager = create_test_pager(f, page_size);

    drive_test_pager(new_page(pager));
    REQUIRE(os::file_get_stats(f).byte_count % page_size == 0);
    destroy_test_pager(pager);
}

TEST_CASE("pager partial writes do not corrupt", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    U64 page_size = 64;
    U64 pidx;
    {
        auto pager = create_test_pager(f, page_size);
        pidx = drive_test_pager(new_page(pager));
        std::vector<U8> buffer(page_size, 0xAA);
        os::memory_copy(drive_test_pager(rwpage(pager, pidx)), buffer.data(), page_size / 2);
        destroy_test_pager(pager);
    }

    auto pager = test_pager(f);
    auto readback = drive_test_pager(rpage(pager, pidx));
    for(U64 i = 0; i < page_size / 2; ++i) REQUIRE(readback[i] == 0xAA);
    destroy_test_pager(pager);
}

TEST_CASE("pager repeated reopen and free consistency", "[plexdb.pager]") {
    os::File f{os::file_tmp()};
    REQUIRE(!os::is_zero_handle(f));

    {
        auto pager = create_test_pager(f, 32);
        U64 pidx = drive_test_pager(new_page(pager));
        drive_test_pager(delete_page(pager, pidx));
        destroy_test_pager(pager);
    }

    {
        auto pager = test_pager(f);
        U64 pidx = drive_test_pager(new_page(pager));
        drive_test_pager(delete_page(pager, pidx));
        destroy_test_pager(pager);
    }
}

// ============================================================================
// WAL tests
// ============================================================================

// Write a page to the database via a WAL-enabled pager and verify it survives
// a reopen with WAL recovery.
TEST_CASE("write and read back via WAL-enabled pager", "[plexdb.pager.wal]") {
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
            pidx = drive_test_pager(new_page(p));
            os::memory_copy(drive_test_pager(rwpage(p, pidx)), data, sizeof(data));
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
            const U8* rb = drive_test_pager(rpage(p, pidx));
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

// Fork a child that commits the WAL but exits immediately (simulating a crash
// before the checkpoint completes). The parent then reopens the pager with the
// WAL and verifies that recovery replays the committed frames.
TEST_CASE("committed WAL replays after abrupt process exit", "[plexdb.pager.wal]") {
    os::Handle pid = os::process_get_handle();
    auto db_path = fmt("/tmp/plexdb_wal::crash_%" PRIu64 "_db",  pid.u64[0]);
    auto wal_path = fmt("/tmp/plexdb_wal::crash_%" PRIu64 "_wal",  pid.u64[0]);

    if (os::file_exists(db_path))  os::file_delete(db_path);
    if (os::file_exists(wal_path)) os::file_delete(wal_path);

    U64 page_size = 128;
    U64 pidx;
    U8 original[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22};
    U8 modified[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};

    // Create database with initial data.(without builtin WAL)
    {
        os::File db{os::file_open(db_path)};
        {
            auto p = create_test_pager(db, page_size);
            pidx = drive_test_pager(new_page(p));
            os::memory_copy(drive_test_pager(rwpage(p, pidx)), original, sizeof(original));
            destroy_test_pager(p);
        }
    }

    // Fork a child that commits the WAL for a modified page, then exits abruptly
    // (no checkpoint). This simulates a crash between WAL commit and DB write.
    auto child_opt = os::process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    os::Handle child = *child_opt;
    if (os::is_zero_handle(child)) {
        // Child process: manually commit WAL without checkpointing.
        os::Handle child_db  = os::file_open(db_path);
        os::Handle child_wal = os::file_open(wal_path);

        Wal wal = create_test_wal(child_wal, page_size);

        U8* buf = os::allocate_zero(page_size);
        os::file_read(child_db, Rng1U64{.start=page_size*pidx, .end=page_size*(pidx + 1)}, buf);
        os::memory_copy(buf, modified, sizeof(modified));
        drive_test_pager(wal::append_frame(wal, g_test_sync_file_io_ctx, pidx, buf));
        drive_test_pager(wal::commit(wal, g_test_sync_file_io_ctx));

        os::deallocate(buf);
        os::process_exit(0);  // crash: no checkpoint
    }
    os::process_wait(child);

    // Parent: reopen pager with WAL — recovery should replay the committed frame.
    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = drive_test_pager(rpage(p, pidx));
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

// Fork a child that appends frames to the WAL but does NOT commit (no
// frame_count update), then exits. The parent verifies that the database is
// unchanged (no partial writes applied).
TEST_CASE("uncommitted frames do not modify the database", "[plexdb.pager.wal]") {
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
            pidx = drive_test_pager(new_page(p));
            os::memory_copy(drive_test_pager(rwpage(p, pidx)), original, sizeof(original));
            destroy_test_pager(p);
        }
    }

    // Child: write frames to WAL but do NOT commit — simulates crash before commit.
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

    // Parent: reopen pager with WAL. No committed frames → original data must be intact.
    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = drive_test_pager(rpage(p, pidx));
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

// Fork a child that commits the WAL, signals readiness via a notifier, then
// waits. The parent sends SIGKILL to interrupt the child after the commit.
// Recovery on reopen must replay the committed WAL frames.
TEST_CASE("SIGKILL after commit triggers WAL recovery", "[plexdb.pager.wal]") {
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
            pidx = drive_test_pager(new_page(p));
            os::memory_copy(drive_test_pager(rwpage(p, pidx)), original, sizeof(original));
            destroy_test_pager(p);
        }
    }

    os::Notifier notifier{};  // pipe for child→parent ready signal

    auto child_opt = os::process_fork();
    REQUIRE(static_cast<bool>(child_opt));
    os::Handle child = *child_opt;
    if (os::is_zero_handle(child)) {
        // Child: commit WAL, signal parent, then wait to be killed.
        os::Handle child_db  = os::file_open(db_path);
        os::Handle child_wal = os::file_open(wal_path);

        Wal wal = create_test_wal(child_wal, page_size);

        U8* buf = os::allocate_zero(page_size);
        os::file_read(child_db, Rng1U64{.start=page_size*pidx, .end=page_size*(pidx + 1)}, buf);
        os::memory_copy(buf, modified, sizeof(modified));
        drive_test_pager(wal::append_frame(wal, g_test_sync_file_io_ctx, pidx, buf));
        drive_test_pager(wal::commit(wal, g_test_sync_file_io_ctx));

        os::deallocate(buf);

        // Signal parent that WAL is committed and we are ready to be killed.
        os::signal_notify_safe(notifier);

        // Suspend until SIGKILL arrives (more efficient than busy-waiting).
        os::process_pause();
    }

    // Parent: wait for child's ready signal then deliver SIGKILL.
    U8 byte = 0;
    os::stream_read(notifier.read, &byte, 1);
    os::signal_send_kill(child);
    os::process_wait(child);

    // Recovery: reopen with WAL; committed frame must be replayed.
    {
        os::Handle db  = os::file_open(db_path);
        os::Handle wal = os::file_open(wal_path);
        {
            auto p = test_pager(db, wal);
            const U8* rb = drive_test_pager(rpage(p, pidx));
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
