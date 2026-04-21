export module plexdb.pager.wal;

import plexdb.base;
import plexdb.os;

export namespace plexdb::pager {
    constexpr Array<U8,8> WAL_MAGIC{'p','X','W','A','L',1,0,0};

    // WAL file layout:
    //   [WalHeader]
    //   [WalFrame] * frame_count   (each frame is sizeof(WalFrame) + page_size bytes)
    //
    // frame_count in the header is the atomic commit point.
    // Writing frame_count > 0 commits the WAL; frame_count == 0 means no committed data.
    // A frame with page_idx == MAX_U64 carries the pager header (padded to page_size).

    struct Wal {
        struct Header {
            Array<U8,8> magic;
            U64 page_size;
            U64 salt;
            U64 frame_count;  // 0 = not committed
        };
        struct Frame {
            U64 page_idx;   // MAX_U64 = pager header frame
            U64 checksum;   // salt ^ page_idx ^ first-8-bytes-of-data
        };

        os::Handle file = os::zero_handle();
        Header header    = {};

        Wal() = default;
        explicit Wal(os::Handle file);
        Wal(Wal&&) noexcept;
        Wal& operator=(Wal&&) noexcept;
        ~Wal();

        Wal(const Wal&)            = delete;
        Wal& operator=(const Wal&) = delete;
    };

    // Write WAL header with page_size and fresh random salt; sets frame_count = 0.
    void wal_init(Wal& wal, U64 page_size);

    // Read the WAL header; returns true if magic and page_size match.
    bool wal_load(Wal& wal, U64 expected_page_size);

    // Returns true if the WAL has a committed (frame_count > 0) transaction.
    bool wal_has_committed(const Wal& wal);

    // Append one page frame to the WAL (does not sync or commit).
    // page_idx == MAX_U64 for the pager header frame.
    void wal_append_frame(Wal& wal, U64 page_idx, const U8* data);

    // Sync pending frames then atomically commit by writing frame_count to the header.
    void wal_commit(Wal& wal);

    // Replay all committed WAL frames into the database file and sync.
    void wal_checkpoint(Wal& wal, os::Handle db_file, U64 base_offset);

    // Reset: set frame_count = 0 in the header so the WAL is no longer committed.
    void wal_reset(Wal& wal);
}
