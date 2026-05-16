export module plexdb.pager.wal;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.coroutine;

import plexdb.pager.types;

export namespace plexdb::wal {
    constexpr Array<U8,8> MAGIC{'p','X','W','A','L',1,0,0};
    constexpr Array<U8,2> CURRENT_VERSION{ 0, 1 };

    struct Header {
        Array<U8,sizeof(MAGIC)> magic;
        Array<U8,sizeof(CURRENT_VERSION)> version;
        U64 page_size;
        U64 salt;
        U64 frame_count;  // 0 = not committed
    };

    struct Frame {
        U64 page_idx;   // MAX_U64 = pager header frame
        U64 checksum;   // salt ^ page_idx ^ first-8-bytes-of-data
    };

    // WAL file layout:
    //   [Header]
    //   [Frame][data] * frame_count   (each frame is sizeof(Frame) + page_size bytes)
    //
    // frame_count in the header is the atomic commit point.
    // Writing frame_count > 0 commits the WAL; frame_count == 0 means no committed data.
    struct Wal {
        os::Handle file = os::zero_handle();
        Header header   = {};

        Wal() = default;
        explicit Wal(os::Handle file);
        explicit Wal(os::Handle file, const Header& header);
        Wal(Wal&&) noexcept;
        Wal& operator=(Wal&&) noexcept;
        ~Wal();

        Wal(const Wal&)            = delete;
        Wal& operator=(const Wal&) = delete;

        operator bool() const { return !os::is_zero_handle(this->file); }
    };

    coroutine::Task<Header> create(aio::FileIOContext& ctx, os::Handle file, U64 page_size);
    coroutine::Task<bool>   try_load(Wal& wal, aio::FileIOContext& ctx, U64 expected_page_size);
    coroutine::Task<bool>   has_committed(const Wal& wal, aio::FileIOContext& ctx);
    coroutine::Task<>       append_frame(Wal& wal, aio::FileIOContext& ctx, U64 page_idx, const U8* data);
    coroutine::Task<>       commit(Wal& wal, aio::FileIOContext& ctx);
    coroutine::Task<>       read_frame(Wal& wal, aio::FileIOContext& ctx, U64 frame_idx, Frame& frame_out, U8* data_out);
    coroutine::Task<>       reset(Wal& wal, aio::FileIOContext& ctx);
}

export namespace plexdb {
    using Wal = wal::Wal;
}
