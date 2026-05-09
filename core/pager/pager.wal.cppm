export module plexdb.pager.wal;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.coroutine;

import plexdb.pager.types;

export namespace plexdb::pager {
    constexpr Array<U8,8> WAL_MAGIC{'p','X','W','A','L',1,0,0};
    constexpr Array<U8,2> WAL_CURRENT_VERSION{ 0, 1 };

    // WAL file layout:
    //   [Header]
    //   [Frame][data] * frame_count   (each frame is sizeof(Frame) + page_size bytes)
    //
    // frame_count in the header is the atomic commit point.
    // Writing frame_count > 0 commits the WAL; frame_count == 0 means no committed data.
    struct Wal {
        struct Header {
            Array<U8,sizeof(WAL_MAGIC)> magic;
            Array<U8,sizeof(WAL_CURRENT_VERSION)> version;
            U64 page_size;
            U64 salt;
            U64 frame_count;  // 0 = not committed
        };
        struct Frame {
            U64 page_idx;   // MAX_U64 = pager header frame
            U64 checksum;   // salt ^ page_idx ^ first-8-bytes-of-data
        };

        os::Handle file = os::zero_handle();
        Header header   = {};

        Wal() = default;
        explicit Wal(os::Handle file);
        Wal(Wal&&) noexcept;
        Wal& operator=(Wal&&) noexcept;
        ~Wal();

        Wal(const Wal&)            = delete;
        Wal& operator=(const Wal&) = delete;

        operator bool() const { return !os::is_zero_handle(this->file); }
    };

    // ========================================================================
    // Sync operations — constructor / crash-recovery paths only.
    // These use blocking OS calls and must NOT be called from a coroutine.
    // ========================================================================
    void wal_create(Wal& wal, U64 page_size);
    bool wal_load(Wal& wal, U64 expected_page_size);
    bool wal_has_committed(const Wal& wal);

    // ========================================================================
    // Async operations — always require a FileIOContext.
    // WAL only writes to its own file; reading frames for checkpoint is the
    // pager's responsibility (see wal_read_frame).
    // ========================================================================
    coroutine::Task<> wal_append_frame(Wal& wal, aio::FileIOContext& ctx, U64 page_idx, const U8* data);
    coroutine::Task<> wal_commit(Wal& wal, aio::FileIOContext& ctx);
    coroutine::Task<> wal_read_frame(Wal& wal, aio::FileIOContext& ctx, U64 frame_idx, Wal::Frame& frame_out, U8* data_out);
    coroutine::Task<> wal_reset(Wal& wal, aio::FileIOContext& ctx);
}
