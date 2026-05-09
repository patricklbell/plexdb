module;
#include "macros.h"
#include <coroutine>
#include <profiling/tracy.hpp>

module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.arena;
import plexdb.aio;
import plexdb.coroutine;

namespace plexdb::pager {
    inline static U64 get_cache_size(Pager& pager) {
        return pager.header.page_size + sizeof(Pager::ReadCacheEntry);
    }
    inline static Pager::ReadCacheEntry* get_cache_entry(Pager& pager, U64 idx) {
        static_assert(sizeof(*pager.read_cache) == 1, "ptr arithmetic");
        U64 cache_idx = idx % pager.read_cache_count;
        return reinterpret_cast<Pager::ReadCacheEntry*>(
            pager.read_cache + cache_idx*get_cache_size(pager)
        );
    }
    inline static U8* get_cache_data(Pager& pager, U64 idx) {
        static_assert(sizeof(*pager.read_cache) == 1, "ptr arithmetic");
        U64 cache_idx = idx % pager.read_cache_count;
        return pager.read_cache + cache_idx*get_cache_size(pager) + sizeof(Pager::ReadCacheEntry);
    }

    // Sync helpers — used only by constructors and maybe_recover_wal.
    static void read_header(Pager& pager) {
        os::file_read(
            pager.file,
            Rng1U64{
                .start=pager.base_offset,
                .end  =pager.base_offset + sizeof(pager.header),
            },
            &pager.header
        );
    }
    static void write_header(Pager& pager) {
        os::file_write(
            pager.file,
            Rng1U64{
                .start=pager.base_offset,
                .end  =pager.base_offset + sizeof(pager.header),
            },
            &pager.header
        );
    }

    Header create(os::Handle file, U64 page_size, U64 base_offset) {
        assert_true(sizeof(Header) <= page_size, "header does not fit in root page");
        assert_true(page_size % sizeof(U64) == 0, "invalid page size alignment");

        Header header{
            .magic = HEADER_MAGIC,
            .version = HEADER_CURRENT_VERSION,
            .page_size=page_size,
            .page_count=1, // header page
            .root_page=MAX_U64,
        };

        os::file_resize_zero(file, base_offset + header.page_count*header.page_size);
        os::file_write(file, Rng1U64{ .start=base_offset, .end=base_offset+sizeof(header) }, &header);

        return header;
    }

    static void init_read_cache(Pager& pager) {
        pager.read_cache = os::allocate_zero(get_cache_size(pager)*pager.read_cache_count);
    }

    // Sync WAL recovery — only called from constructors, uses blocking OS calls.
    // Pager writes the DB pages; WAL provides only its own file handle here.
    static void maybe_recover_wal(Pager& pager) {
        if (!pager.wal) return;
        if (!wal_load(pager.wal, pager.header.page_size)) return;
        if (!wal_has_committed(pager.wal)) return;

        U64 page_size = pager.wal.header.page_size;
        U8* buf = os::allocate(max(page_size, static_cast<U64>(sizeof(Header))));

        for (U64 i = 0; i < pager.wal.header.frame_count; i++) {
            U64 offset = sizeof(Wal::Header) + i * (sizeof(Wal::Frame) + page_size);

            Wal::Frame frame{};
            os::file_read(pager.wal.file, Rng1U64{.start=offset, .end=offset+sizeof(Wal::Frame)}, &frame);

            U64 data_size = frame.page_idx == MAX_U64 ? sizeof(Header) : page_size;
            os::file_read(pager.wal.file, Rng1U64{.start=offset+sizeof(Wal::Frame), .end=offset+sizeof(Wal::Frame)+data_size}, buf);

            if (frame.page_idx == MAX_U64) {
                os::file_write(pager.file, Rng1U64{.start=pager.base_offset, .end=pager.base_offset+sizeof(Header)}, buf);
            } else {
                U64 db_start = pager.base_offset + page_size * frame.page_idx;
                os::file_write(pager.file, Rng1U64{.start=db_start, .end=db_start+page_size}, buf);
            }
        }
        os::file_sync(pager.file);

        os::deallocate(buf);

        // Reset WAL synchronously.
        pager.wal.header.frame_count = 0;
        os::file_write(pager.wal.file,
            Rng1U64{
                .start=offsetof(Wal::Header, frame_count),
                .end  =offsetof(Wal::Header, frame_count)+sizeof(U64)
            },
            &pager.wal.header.frame_count);
        os::file_sync(pager.wal.file);

        read_header(pager);
    }

    Pager::Pager(os::Handle file, U64 base_offset, U64 read_cache)
      : file(file), base_offset(base_offset), read_cache_count(read_cache) {
        read_header(*this);
        init_read_cache(*this);
    }
    Pager::Pager(os::Handle file, const Header& header, U64 base_offset, U64 read_cache)
       : file(file), base_offset(base_offset), header(header), read_cache_count(read_cache) {
        init_read_cache(*this);
    }

    Pager::Pager(os::Handle file, os::Handle wal_file, U64 base_offset, U64 read_cache)
      : file(file), base_offset(base_offset), read_cache_count(read_cache), wal(wal_file) {
        read_header(*this);
        maybe_recover_wal(*this);
        init_read_cache(*this);
    }
    Pager::Pager(os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset, U64 read_cache)
       : file(file), base_offset(base_offset), header(header), read_cache_count(read_cache), wal(wal_file) {
        wal_create(this->wal, header.page_size);
        init_read_cache(*this);
    }

    Pager::Pager(Pager&& other):
        file(other.file),
        base_offset(other.base_offset),
        file_io_ctx(other.file_io_ctx),
        header(other.header),
        header_in_write_set(other.header_in_write_set),
        read_cache_count(other.read_cache_count),
        read_cache(other.read_cache),
        write_arena(move(other.write_arena)),
        write_set(move(other.write_set)),
        wal(move(other.wal))
    {
        other.file = os::zero_handle();
        other.file_io_ctx = nullptr;
        other.read_cache = nullptr;
    }

    // Sync flush for destructor/move-assignment — exempted from the async requirement.
    static void sync_flush(Pager& pager) {
        if (os::is_zero_handle(pager.file)) return;

        if (pager.wal) {
            // Full WAL path: append frames, commit, checkpoint, reset — all sync.
            U64 page_size = pager.wal.header.page_size;
            if (page_size == 0) page_size = pager.header.page_size;

            for (auto& idx : pager.write_set) {
                auto entry = get_cache_entry(pager, idx);
                if (idx < pager.header.page_count) {
                    U64 offset = sizeof(Wal::Header) + pager.wal.header.frame_count * (sizeof(Wal::Frame) + page_size);
                    Wal::Frame frame{};
                    frame.page_idx = idx;
                    U64 dw = 0; os::memory_copy(&dw, get_cache_data(pager, idx), sizeof(U64));
                    frame.checksum = pager.wal.header.salt ^ idx ^ dw;
                    os::file_resize_zero(pager.wal.file, offset + sizeof(Wal::Frame) + page_size);
                    os::file_write(pager.wal.file, Rng1U64{.start=offset, .end=offset+sizeof(Wal::Frame)}, &frame);
                    os::file_write(pager.wal.file, Rng1U64{.start=offset+sizeof(Wal::Frame), .end=offset+sizeof(Wal::Frame)+page_size}, get_cache_data(pager, idx));
                    pager.wal.header.frame_count++;
                }
                entry->in_write_set = false;
            }
            if (pager.header_in_write_set) {
                U64 offset = sizeof(Wal::Header) + pager.wal.header.frame_count * (sizeof(Wal::Frame) + page_size);
                Wal::Frame frame{};
                frame.page_idx = MAX_U64;
                U64 dw = 0; os::memory_copy(&dw, &pager.header, sizeof(U64));
                frame.checksum = pager.wal.header.salt ^ MAX_U64 ^ dw;
                os::file_resize_zero(pager.wal.file, offset + sizeof(Wal::Frame) + sizeof(Header));
                os::file_write(pager.wal.file, Rng1U64{.start=offset, .end=offset+sizeof(Wal::Frame)}, &frame);
                os::file_write(pager.wal.file, Rng1U64{.start=offset+sizeof(Wal::Frame), .end=offset+sizeof(Wal::Frame)+sizeof(Header)}, &pager.header);
                pager.wal.header.frame_count++;
                pager.header_in_write_set = false;
            }

            if (pager.wal.header.frame_count > 0) {
                // Commit WAL.
                os::file_sync(pager.wal.file);
                os::file_write(pager.wal.file,
                    Rng1U64{.start=offsetof(Wal::Header, frame_count), .end=offsetof(Wal::Header, frame_count)+sizeof(U64)},
                    &pager.wal.header.frame_count);
                os::file_sync(pager.wal.file);

                // Checkpoint: pager writes DB pages.
                U8* buf = os::allocate(max(page_size, static_cast<U64>(sizeof(Header))));
                for (U64 i = 0; i < pager.wal.header.frame_count; i++) {
                    U64 offset = sizeof(Wal::Header) + i * (sizeof(Wal::Frame) + page_size);
                    Wal::Frame frame{};
                    os::file_read(pager.wal.file, Rng1U64{.start=offset, .end=offset+sizeof(Wal::Frame)}, &frame);
                    U64 data_size = frame.page_idx == MAX_U64 ? sizeof(Header) : page_size;
                    os::file_read(pager.wal.file, Rng1U64{.start=offset+sizeof(Wal::Frame), .end=offset+sizeof(Wal::Frame)+data_size}, buf);

                    if (frame.page_idx == MAX_U64) {
                        os::file_write(pager.file, Rng1U64{.start=pager.base_offset, .end=pager.base_offset+sizeof(Header)}, buf);
                    } else {
                        U64 db_start = pager.base_offset + page_size * frame.page_idx;
                        os::file_write(pager.file, Rng1U64{.start=db_start, .end=db_start+page_size}, buf);
                    }
                }
                os::deallocate(buf);
                os::file_sync(pager.file);

                // Reset WAL.
                pager.wal.header.frame_count = 0;
                os::file_write(pager.wal.file,
                    Rng1U64{.start=offsetof(Wal::Header, frame_count), .end=offsetof(Wal::Header, frame_count)+sizeof(U64)},
                    &pager.wal.header.frame_count);
                os::file_sync(pager.wal.file);
            }
        } else {
            for (auto& idx : pager.write_set) {
                auto entry = get_cache_entry(pager, idx);
                if (idx < pager.header.page_count) {
                    U64 offset = pager.base_offset + pager.header.page_size * idx;
                    os::file_write(pager.file, Rng1U64{.start=offset, .end=offset+pager.header.page_size}, get_cache_data(pager, idx));
                }
                entry->in_write_set = false;
            }
            if (pager.header_in_write_set) {
                write_header(pager);
                pager.header_in_write_set = false;
            }
            os::file_sync(pager.file);
        }

        clear(pager.write_set);
        arena::clear(pager.write_arena);
    }

    Pager::~Pager() {
        sync_flush(*this);
        os::deallocate(this->read_cache);
    }

    Pager& Pager::operator=(Pager&& other) {
        if (this == &other) {
            return *this;
        }

        sync_flush(*this);
        os::deallocate(this->read_cache);

        file = other.file;
        base_offset = other.base_offset;
        file_io_ctx = other.file_io_ctx;
        header = other.header;
        header_in_write_set = other.header_in_write_set;
        read_cache_count = other.read_cache_count;
        read_cache = other.read_cache;
        write_arena = move(other.write_arena);
        write_set = move(other.write_set);
        wal = move(other.wal);

        other.file = os::zero_handle();
        other.file_io_ctx = nullptr;
        other.read_cache = nullptr;

        return *this;
    }

    void set_root(Pager& pager, U64 page) {
        pager.header.root_page = page;
        pager.header_in_write_set = true;
    }

    coroutine::Task<const U8*> rpage(Pager& pager, U64 idx) {
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rpage requires file_io_ctx");

        auto entry = get_cache_entry(pager, idx);
        if (entry->idx == idx)
            co_return get_cache_data(pager, idx);
        if (entry->in_write_set)
            co_await fflush(pager);

        auto data = get_cache_data(pager, idx);
        auto offset = pager.base_offset + pager.header.page_size * idx;
        auto err = co_await aio::file_read(*pager.file_io_ctx, pager.file, offset, static_cast<U32>(pager.header.page_size), data);
        assert_true(err == aio::Error::None, "async page read failed");
        entry->idx = idx;
        co_return data;
    }

    coroutine::Task<U8*> rwpage(Pager& pager, U64 idx) {
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rwpage requires file_io_ctx");

        auto entry = get_cache_entry(pager, idx);

        if (entry->idx != 0 && entry->idx != idx && entry->in_write_set)
            co_await fflush(pager);
        if (!entry->in_write_set)
            push_front(pager.write_arena, pager.write_set, idx);
        if (entry->idx != idx) {
            auto data = get_cache_data(pager, idx);
            auto offset = pager.base_offset + pager.header.page_size * idx;
            auto err = co_await aio::file_read(*pager.file_io_ctx, pager.file, offset, static_cast<U32>(pager.header.page_size), data);
            assert_true(err == aio::Error::None, "async page read failed");
        }

        entry->idx = idx;
        entry->in_write_set = true;
        co_return get_cache_data(pager, idx);
    }

    coroutine::Task<> fflush(Pager& pager) { ZoneScopedN("pager::fflush");
        assert_true(pager.file_io_ctx != nullptr, "fflush requires file_io_ctx");
        aio::FileIOContext& ctx = *pager.file_io_ctx;

        if (pager.wal) {
            assert_true(pager.wal.header.page_size != 0, "invalid wal page size");

            U64 frames_appended = 0;
            for (auto& idx : pager.write_set) {
                auto entry = get_cache_entry(pager, idx);
                assert_true(entry->idx != 0, "header page found in write set");
                if (idx < pager.header.page_count) {
                    co_await wal_append_frame(pager.wal, ctx, idx, get_cache_data(pager, idx));
                    frames_appended++;
                }
                entry->in_write_set = false;
            }
            if (pager.header_in_write_set) {
                co_await wal_append_frame(pager.wal, ctx, MAX_U64, reinterpret_cast<U8*>(&pager.header));
                pager.header_in_write_set = false;
                frames_appended++;
            }

            if (frames_appended > 0) {
                co_await wal_commit(pager.wal, ctx);

                // Checkpoint: pager reads WAL frames and writes DB pages.
                U64 page_size = pager.wal.header.page_size;
                U8* buf = os::allocate(max(page_size, static_cast<U64>(sizeof(Header))));

                for (U64 i = 0; i < pager.wal.header.frame_count; i++) {
                    Wal::Frame frame{};
                    co_await wal_read_frame(pager.wal, ctx, i, frame, buf);

                    if (frame.page_idx == MAX_U64) {
                        auto err = co_await aio::file_write(ctx, pager.file, pager.base_offset, static_cast<U32>(sizeof(Header)), buf);
                        assert_true(err == aio::Error::None, "async checkpoint header write failed");
                    } else {
                        U64 db_start = pager.base_offset + page_size * frame.page_idx;
                        auto err = co_await aio::file_write(ctx, pager.file, db_start, static_cast<U32>(page_size), buf);
                        assert_true(err == aio::Error::None, "async checkpoint page write failed");
                    }
                }

                os::deallocate(buf);

                auto err = co_await aio::file_sync(ctx, pager.file);
                assert_true(err == aio::Error::None, "async checkpoint db sync failed");

                co_await wal_reset(pager.wal, ctx);
            }
        } else {
            for (auto& idx : pager.write_set) {
                auto entry = get_cache_entry(pager, idx);
                assert_true(entry->idx != 0, "header page found in write set");
                if (idx < pager.header.page_count) {
                    U64 offset = pager.base_offset + pager.header.page_size * idx;
                    auto err = co_await aio::file_write(ctx, pager.file, offset, static_cast<U32>(pager.header.page_size), get_cache_data(pager, idx));
                    assert_true(err == aio::Error::None, "async page write failed");
                }
                entry->in_write_set = false;
            }
            if (pager.header_in_write_set) {
                auto err = co_await aio::file_write(ctx, pager.file,
                    pager.base_offset, static_cast<U32>(sizeof(pager.header)),
                    reinterpret_cast<const U8*>(&pager.header));
                assert_true(err == aio::Error::None, "async header write failed");
                pager.header_in_write_set = false;
            }
            auto err = co_await aio::file_sync(ctx, pager.file);
            assert_true(err == aio::Error::None, "async file sync failed");
        }

        clear(pager.write_set);
        arena::clear(pager.write_arena);
    }

    // bitset is stored as a page of U64s at the beginning of a block eg.
    // +--------+----------+--------+--------+-----------+--------+----------+------------+
    // | header | bitset 0 | page 2 | page 3 |    ...    | page x | bitset 1 |    ...     |
    // +--------+----------+--------+--------+-----------+--------+----------+------------+
    // if occupied pages are 0, 1, 8, and 68, relative to the bitset's page, then the
    // bitset stored is represented as the following in little endian byte order
    // +----------------------------------------------------------------------------------+
    // | 00000011 00000001 00000000 00000000 00000000 00000000 00000000 00000000 00000000 |
    // +----------------------------------------------------------------------------------+
    // | 00010000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 |
    // +----------------------------------------------------------------------------------+
    //                                         ....
    constexpr inline U64 get_bitset_entry_count(Pager& pager) {
        static_assert(sizeof(pager.header) > sizeof(U64));
        return pager.header.page_size / sizeof(U64);
    }
    constexpr inline U64 get_pages_per_entry() {
        return sizeof(U64)*8;
    }
    inline static coroutine::Task<const U64*> rbitset(Pager& pager, U64 idx) {
        co_return reinterpret_cast<const U64*>(co_await rpage(pager, idx));
    }
    inline static coroutine::Task<U64*> rwbitset(Pager& pager, U64 idx) {
        co_return reinterpret_cast<U64*>(co_await rwpage(pager, idx));
    }

    coroutine::Task<U64> new_page(Pager& pager) { ZoneScopedN("pager::new_page");
        const U64 ENTRIES_PER_BITSET = get_bitset_entry_count(pager);
        const U64 BITSET_PAGE_STRIDE = ENTRIES_PER_BITSET*get_pages_per_entry() + 1;
        auto& h = pager.header;

        for (U64 bitset_page = 1; bitset_page < h.page_count; bitset_page += BITSET_PAGE_STRIDE) {
            U64* bitset = co_await rwbitset(pager, bitset_page);

            for (U64 entry_idx = 0; entry_idx < ENTRIES_PER_BITSET; entry_idx++) {
                if (~bitset[entry_idx] != 0) {
                    U64 bit_idx = bit_count_trailing_zeros(~bitset[entry_idx]);
                    bitset[entry_idx] |= 1_u64 << bit_idx;

                    U64 free_page = bitset_page + 1 + get_pages_per_entry()*entry_idx + bit_idx;
                    if (free_page >= h.page_count) {
                        h.page_count = free_page+1;
                        pager.header_in_write_set = true;
                        os::file_resize_zero(pager.file, pager.base_offset + h.page_count*h.page_size);
                    }
                    co_return free_page;
                }
            }
        }

        assert_true((h.page_count - 1) % BITSET_PAGE_STRIDE == 0, "bitset correctly aligned");

        U64 bitset_page = h.page_count++;
        U64 free_page = h.page_count++;
        pager.header_in_write_set = true;
        os::file_resize_zero(pager.file, pager.base_offset + h.page_count*h.page_size);

        U64* bitset = co_await rwbitset(pager, bitset_page);
        bitset[0] |= 1_u64;

        co_return free_page;
    }

    coroutine::Task<> delete_page(Pager& pager, U64 idx) { ZoneScopedN("pager::delete_page");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        const U64 PAGES_PER_BITSET = get_bitset_entry_count(pager)*get_pages_per_entry() + 1;
        auto& h = pager.header;

        U64 bitset_page = ((idx - 1)/PAGES_PER_BITSET)*PAGES_PER_BITSET + 1;
        assert_true(idx != bitset_page, "invalid delete page");
        U64* bitset = co_await rwbitset(pager, bitset_page);

        U64 bitset_idx = idx - bitset_page - 1;
        U64 entry_idx  = bitset_idx / get_pages_per_entry();
        U64 bit_idx    = bitset_idx % get_pages_per_entry();
        assert_true(bitset[entry_idx] & 1_u64 << bit_idx, "trying to delete page marked as deleted");
        bitset[entry_idx] &= ~(1_u64 << bit_idx);

        if (idx != h.page_count - 1)
            co_return;

        const U64* bitset_r = bitset;
        PLEXDB_DEBUG_X(U64 old_page_count = h.page_count);
        while (true) {
            for (;; entry_idx--) {
                if (bitset_r[entry_idx] != 0) {
                    U64 occupied_count = get_pages_per_entry() - bit_count_leading_zeros(bitset_r[entry_idx]);
                    h.page_count = bitset_page + 1 + entry_idx*get_pages_per_entry() + occupied_count;
                    break;
                }
                if (entry_idx == 0) {
                    h.page_count = bitset_page;
                    break;
                }
            }

            if (h.page_count != bitset_page || h.page_count <= PAGES_PER_BITSET)
                break;
            bitset_page -= PAGES_PER_BITSET;
            bitset_r = co_await rbitset(pager, bitset_page);
            entry_idx = get_bitset_entry_count(pager) - 1;
        }
        PLEXDB_DEBUG_X(assert_true(h.page_count < old_page_count, "trimming actually trimmed"));
        os::file_resize_zero(pager.file, pager.base_offset + h.page_count*h.page_size);
        pager.header_in_write_set = true;
    }
}
