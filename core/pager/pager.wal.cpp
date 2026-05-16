module;
#include <plexdb/macros/macros.h>
#include <coroutine>

module plexdb.pager.wal;

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.arena;
import plexdb.aio;
import plexdb.coroutine;

import plexdb.pager.types;

namespace plexdb::wal {
    static constexpr U64 HEADER_FRAME_IDX = MAX_U64;

    static U64 frame_checksum(U64 salt, U64 page_idx, const U8* data) {
        U64 data_word = 0;
        os::memory_copy(&data_word, data, sizeof(U64));
        return salt ^ page_idx ^ data_word;
    }

    static U64 frame_offset(const Wal& wal, U64 frame_idx) {
        return sizeof(Header) + frame_idx * (sizeof(Frame) + wal.header.page_size);
    }

    Wal::Wal(os::Handle file) : file(file) {}

    Wal::Wal(os::Handle file, const Header& header) : file(file), header(header) {}

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

    coroutine::Task<Header> create(aio::FileIOContext& ctx, os::Handle file, U64 page_size) {
        assert_true(!os::is_zero_handle(file), "wal::create requires valid file handle");

        Header header{
            .magic       = MAGIC,
            .version     = CURRENT_VERSION,
            .page_size   = page_size,
            .salt        = static_cast<U64>(os::monotonic_us()) ^ file.u64[0],
            .frame_count = 0,
        };
        os::file_resize_zero(file, sizeof(Header));
        co_await aio::file_write(ctx, file, Rng1U64{.start=0, .end=sizeof(Header)}, &header);
        co_await aio::file_sync(ctx, file);

        co_return header;
    }

    coroutine::Task<bool> try_load(Wal& wal, aio::FileIOContext& ctx, U64 expected_page_size) {
        assert_true(!os::is_zero_handle(wal.file), "wal::try_load requires valid file handle");

        os::FileStats stats = os::file_get_stats(wal.file);
        if (stats.byte_count < sizeof(Header)) {
            co_return false;
        }

        Header h{};
        co_await aio::file_read(ctx, wal.file, Rng1U64{.start=0, .end=sizeof(Header)}, &h);

        if (h.magic != MAGIC)              co_return false;
        if (h.page_size != expected_page_size) co_return false;

        wal.header = h;
        co_return true;
    }

    coroutine::Task<bool> has_committed(const Wal& wal, [[maybe_unused]] aio::FileIOContext& ctx) {
        co_return wal.header.frame_count > 0;
    }

    coroutine::Task<> append_frame(Wal& wal, aio::FileIOContext& ctx, U64 page_idx, const U8* data) {
        assert_true(!os::is_zero_handle(wal.file), "wal::append_frame requires valid file handle");

        U64 offset = frame_offset(wal, wal.header.frame_count);
        Frame frame{
            .page_idx = page_idx,
            .checksum = frame_checksum(wal.header.salt, page_idx, data),
        };

        U64 data_size = page_idx == HEADER_FRAME_IDX ? sizeof(Header) : wal.header.page_size;
        os::file_resize_zero(wal.file, offset + sizeof(Frame) + data_size);

        co_await aio::file_write(
            ctx,
            wal.file,
            Rng1U64{.start=offset, .end=offset + sizeof(Frame)},
            &frame
        );
        co_await aio::file_write(
            ctx,
            wal.file,
            Rng1U64{.start=offset + sizeof(Frame), .end=offset + sizeof(Frame) + data_size},
            data
        );

        wal.header.frame_count++;
    }

    coroutine::Task<> commit(Wal& wal, aio::FileIOContext& ctx) {
        assert_true(!os::is_zero_handle(wal.file), "wal::commit requires valid file handle");
        assert_true(wal.header.frame_count > 0, "nothing to commit");

        co_await aio::file_sync(ctx, wal.file);
        co_await aio::file_write(
            ctx,
            wal.file,
            Rng1U64{.start=offsetof(Header, frame_count), .end=offsetof(Header, frame_count) + sizeof(U64)},
            &wal.header.frame_count
        );
        co_await aio::file_sync(ctx, wal.file);
    }

    coroutine::Task<> read_frame(Wal& wal, aio::FileIOContext& ctx, U64 frame_idx, Frame& frame_out, U8* data_out) {
        assert_true(!os::is_zero_handle(wal.file), "wal::read_frame requires valid file handle");
        assert_true(frame_idx < wal.header.frame_count, "wal frame index out of range");

        U64 offset = frame_offset(wal, frame_idx);
        U64 data_size = wal.header.page_size;

        co_await aio::file_read(
            ctx,
            wal.file,
            Rng1U64{.start=offset, .end=offset + sizeof(Frame)},
            reinterpret_cast<U8*>(&frame_out)
        );

        if (frame_out.page_idx == HEADER_FRAME_IDX) {
            data_size = sizeof(Header);
        }

        co_await aio::file_read(
            ctx,
            wal.file,
            Rng1U64{.start=offset + sizeof(Frame), .end=offset + sizeof(Frame) + data_size},
            data_out
        );

        U64 expected = frame_checksum(wal.header.salt, frame_out.page_idx, data_out);
        assert_true(frame_out.checksum == expected, "WAL frame checksum mismatch");
    }

    coroutine::Task<> reset(Wal& wal, aio::FileIOContext& ctx) {
        assert_true(!os::is_zero_handle(wal.file), "wal::reset requires valid file handle");

        wal.header.frame_count = 0;

        co_await aio::file_write(
            ctx,
            wal.file,
            Rng1U64{.start=offsetof(Header, frame_count), .end=offsetof(Header, frame_count) + sizeof(U64)},
            &wal.header.frame_count
        );
        co_await aio::file_sync(ctx, wal.file);
    }
}
