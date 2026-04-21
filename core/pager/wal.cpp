module;
#include "macros.h"

module plexdb.pager.wal;

import plexdb.base;
import plexdb.os;

namespace plexdb::pager {
    static constexpr U64 WAL_HEADER_FRAME_IDX = MAX_U64;

    // Checksum: salt XOR page_idx XOR first 8 bytes of page data.
    static U64 frame_checksum(U64 salt, U64 page_idx, const U8* data) {
        U64 data_word = 0;
        os::memory_copy(&data_word, data, sizeof(U64));  // always 8 bytes
        return salt ^ page_idx ^ data_word;
    }

    // Byte offset of frame N inside the WAL file.
    static U64 frame_offset(const Wal& wal, U64 frame_idx) {
        return sizeof(Wal::Header) + frame_idx * (sizeof(Wal::Frame) + wal.header.page_size);
    }

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

    Wal::~Wal() {} // caller manages the file handle lifetime

    void wal_init(Wal& wal, U64 page_size) {
        assert_true(!os::is_zero_handle(wal.file), "wal_init requires valid file handle");

        // @note Use monotonic time XOR address as a non-cryptographic salt for
        // frame checksum uniqueness within a WAL lifecycle (not for security).
        U64 salt = static_cast<U64>(os::monotonic_us()) ^ (U64)(uintptr_t)(&wal);

        wal.header = Wal::Header{
            .magic       = WAL_MAGIC,
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

        if (h.magic != WAL_MAGIC)           return false;
        if (h.page_size != expected_page_size) return false;

        wal.header = h;
        return true;
    }

    bool wal_has_committed(const Wal& wal) {
        return wal.header.frame_count > 0;
    }

    void wal_append_frame(Wal& wal, U64 page_idx, const U8* data) {
        assert_true(!os::is_zero_handle(wal.file), "wal_append_frame requires valid file handle");

        U64 offset = frame_offset(wal, wal.header.frame_count);
        Wal::Frame frame{
            .page_idx = page_idx,
            .checksum = frame_checksum(wal.header.salt, page_idx, data),
        };

        // Grow file to accommodate this frame.
        U64 end = offset + sizeof(Wal::Frame) + wal.header.page_size;
        os::file_resize_zero(wal.file, end);

        os::file_write(wal.file, Rng1U64{.start=offset, .end=offset+sizeof(Wal::Frame)}, &frame);
        os::file_write(
            wal.file,
            Rng1U64{.start=offset+sizeof(Wal::Frame), .end=end},
            data
        );

        wal.header.frame_count++;
    }

    void wal_commit(Wal& wal) {
        assert_true(!os::is_zero_handle(wal.file), "wal_commit requires valid file handle");
        assert_true(wal.header.frame_count > 0, "nothing to commit");

        // Sync all frame data before writing the commit counter.
        os::file_sync(wal.file);

        // Write frame_count: this is the atomic commit point.
        os::file_write(
            wal.file,
            Rng1U64{
                .start = offsetof(Wal::Header, frame_count),
                .end   = offsetof(Wal::Header, frame_count) + sizeof(U64),
            },
            &wal.header.frame_count
        );
        os::file_sync(wal.file);
    }

    void wal_checkpoint(Wal& wal, os::Handle db_file, U64 base_offset) {
        assert_true(!os::is_zero_handle(wal.file), "wal_checkpoint requires valid file handle");
        assert_true(wal_has_committed(wal), "wal_checkpoint: no committed data");

        U64 page_size = wal.header.page_size;
        U8* buf = os::allocate(page_size);

        for (U64 i = 0; i < wal.header.frame_count; i++) {
            U64 offset = frame_offset(wal, i);

            Wal::Frame frame{};
            os::file_read(wal.file, Rng1U64{.start=offset, .end=offset+sizeof(Wal::Frame)}, &frame);
            os::file_read(wal.file, Rng1U64{.start=offset+sizeof(Wal::Frame), .end=offset+sizeof(Wal::Frame)+page_size}, buf);

            U64 expected = frame_checksum(wal.header.salt, frame.page_idx, buf);
            assert_true(frame.checksum == expected, "WAL frame checksum mismatch during checkpoint");

            if (frame.page_idx == WAL_HEADER_FRAME_IDX) {
                // Pager header frame: padded to page_size on write, so write the
                // full buffer here (safe — pager only reads sizeof(Pager::Header) bytes).
                os::file_write(db_file, Rng1U64{.start=base_offset, .end=base_offset+page_size}, buf);
            } else {
                U64 db_start = base_offset + page_size * frame.page_idx;
                os::file_write(db_file, Rng1U64{.start=db_start, .end=db_start+page_size}, buf);
            }
        }

        os::deallocate(buf);
        os::file_sync(db_file);
    }

    void wal_reset(Wal& wal) {
        assert_true(!os::is_zero_handle(wal.file), "wal_reset requires valid file handle");

        wal.header.frame_count = 0;
        os::file_write(
            wal.file,
            Rng1U64{
                .start = offsetof(Wal::Header, frame_count),
                .end   = offsetof(Wal::Header, frame_count) + sizeof(U64),
            },
            &wal.header.frame_count
        );
        os::file_sync(wal.file);
    }
}
