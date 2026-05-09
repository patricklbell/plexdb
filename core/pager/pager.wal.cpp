module;
#include "macros.h"
#include <coroutine>

module plexdb.pager.wal;

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.arena;
import plexdb.aio;
import plexdb.coroutine;

import plexdb.pager.types;

namespace plexdb::pager {
    static constexpr U64 WAL_HEADER_FRAME_IDX = MAX_U64;

    static U64 frame_checksum(U64 salt, U64 page_idx, const U8* data) {
        U64 data_word = 0;
        os::memory_copy(&data_word, data, sizeof(U64));
        return salt ^ page_idx ^ data_word;
    }

    static U64 frame_offset(const Wal& wal, U64 frame_idx) {
        return sizeof(Wal::Header) + frame_idx * (sizeof(Wal::Frame) + wal.header.page_size);
    }

    // ========================================================================
    // constructors
    // ========================================================================

    Wal::Wal(os::Handle file) : file(file) {}

    Wal::Wal(Wal&& other) noexcept : file(other.file), header(other.header) {
        other.file = os::zero_handle();
    }

    Wal& Wal::operator=(Wal&& other) noexcept {
        if (this != &other) {
            this->file   = other.file;
            this->header = other.header;
            other.file   = os::zero_handle();
        }
        return *this;
    }

    Wal::~Wal() {}

    // ========================================================================
    // sync operations (constructor / crash-recovery paths)
    // ========================================================================

    void wal_create(Wal& wal, U64 page_size) {
        assert_true(!os::is_zero_handle(wal.file), "wal_create requires valid file handle");

        U64 salt = static_cast<U64>(os::monotonic_us()) ^ (U64)(uintptr_t)(&wal);

        wal.header = Wal::Header{
            .magic       = WAL_MAGIC,
            .version     = WAL_CURRENT_VERSION,
            .page_size   = page_size,
            .salt        = salt,
            .frame_count = 0,
        };
        os::file_resize_zero(wal.file, sizeof(Wal::Header));
        os::file_write(wal.file, Rng1U64{.start=0, .end=sizeof(Wal::Header)}, &wal.header);
        os::file_sync(wal.file);
    }

    bool wal_load(Wal& wal, U64 expected_page_size) {
        assert_true(!os::is_zero_handle(wal.file), "wal_load requires valid file handle");

        os::FileStats stats = os::file_get_stats(wal.file);
        if (stats.byte_count < sizeof(Wal::Header))
            return false;

        Wal::Header h{};
        os::file_read(wal.file, Rng1U64{.start=0, .end=sizeof(Wal::Header)}, &h);

        if (h.magic != WAL_MAGIC)              return false;
        if (h.page_size != expected_page_size) return false;

        wal.header = h;
        return true;
    }

    bool wal_has_committed(const Wal& wal) {
        return wal.header.frame_count > 0;
    }

    // ========================================================================
    // async operations — WAL writes to its own file only
    // ========================================================================

    coroutine::Task<> wal_append_frame(Wal& wal, aio::FileIOContext& ctx, U64 page_idx, const U8* data) {
        assert_true(!os::is_zero_handle(wal.file), "wal_append_frame requires valid file handle");

        U64 offset = frame_offset(wal, wal.header.frame_count);
        Wal::Frame frame{
            .page_idx = page_idx,
            .checksum = frame_checksum(wal.header.salt, page_idx, data),
        };

        U64 data_size = page_idx == WAL_HEADER_FRAME_IDX ? sizeof(Header) : wal.header.page_size;
        os::file_resize_zero(wal.file, offset + sizeof(Wal::Frame) + data_size);

        auto err = co_await aio::file_write(ctx, wal.file, offset, sizeof(Wal::Frame), reinterpret_cast<const U8*>(&frame));
        assert_true(err == aio::Error::None, "async wal frame header write failed");

        err = co_await aio::file_write(ctx, wal.file, offset + sizeof(Wal::Frame), static_cast<U32>(data_size), data);
        assert_true(err == aio::Error::None, "async wal frame data write failed");

        wal.header.frame_count++;
    }

    coroutine::Task<> wal_commit(Wal& wal, aio::FileIOContext& ctx) {
        assert_true(!os::is_zero_handle(wal.file), "wal_commit requires valid file handle");
        assert_true(wal.header.frame_count > 0, "nothing to commit");

        auto err = co_await aio::file_sync(ctx, wal.file);
        assert_true(err == aio::Error::None, "async wal pre-commit sync failed");

        err = co_await aio::file_write(ctx, wal.file,
            offsetof(Wal::Header, frame_count), sizeof(U64),
            reinterpret_cast<const U8*>(&wal.header.frame_count));
        assert_true(err == aio::Error::None, "async wal commit write failed");

        err = co_await aio::file_sync(ctx, wal.file);
        assert_true(err == aio::Error::None, "async wal post-commit sync failed");
    }

    coroutine::Task<> wal_read_frame(Wal& wal, aio::FileIOContext& ctx, U64 frame_idx, Wal::Frame& frame_out, U8* data_out) {
        assert_true(!os::is_zero_handle(wal.file), "wal_read_frame requires valid file handle");
        assert_true(frame_idx < wal.header.frame_count, "wal frame index out of range");

        U64 offset = frame_offset(wal, frame_idx);
        U64 data_size = wal.header.page_size;

        auto err = co_await aio::file_read(ctx, wal.file, offset, sizeof(Wal::Frame), reinterpret_cast<U8*>(&frame_out));
        assert_true(err == aio::Error::None, "async wal frame header read failed");

        if (frame_out.page_idx == WAL_HEADER_FRAME_IDX)
            data_size = sizeof(Header);

        err = co_await aio::file_read(ctx, wal.file, offset + sizeof(Wal::Frame), static_cast<U32>(data_size), data_out);
        assert_true(err == aio::Error::None, "async wal frame data read failed");

        U64 expected = frame_checksum(wal.header.salt, frame_out.page_idx, data_out);
        assert_true(frame_out.checksum == expected, "WAL frame checksum mismatch");
    }

    coroutine::Task<> wal_reset(Wal& wal, aio::FileIOContext& ctx) {
        assert_true(!os::is_zero_handle(wal.file), "wal_reset requires valid file handle");

        wal.header.frame_count = 0;
        auto err = co_await aio::file_write(ctx, wal.file,
            offsetof(Wal::Header, frame_count), sizeof(U64),
            reinterpret_cast<const U8*>(&wal.header.frame_count));
        assert_true(err == aio::Error::None, "async wal reset write failed");

        err = co_await aio::file_sync(ctx, wal.file);
        assert_true(err == aio::Error::None, "async wal reset sync failed");
    }
}
