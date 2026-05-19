module;
#include <plexdb/macros/macros.h>
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.plugin;
import plexdb.threads;
import plexdb.arena;
import plexdb.aio;

namespace plexdb::pager {
    static plugin::Producer pager_producer{"plexdb::pager"};

    static U64 page_cache_hash(U64 key) {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return key;
    }

    static PageCache::Slot* page_cache_find(PageCache& cache, U64 key) {
        if (!cache.slots) return nullptr;
        U64 h = page_cache_hash(key) & (cache.slot_count - 1);
        for (U64 i = 0; i < cache.slot_count; i++) {
            U64 idx = (h + i) & (cache.slot_count - 1);
            if (cache.slots[idx].key == PageCache::EMPTY) return nullptr;
            if (cache.slots[idx].key == key) return &cache.slots[idx];
        }
        return nullptr;
    }

    static void page_cache_grow(PageCache& cache, Arena& arena) {
        U64 new_count = cache.slot_count == 0 ? 16 : cache.slot_count * 2;
        auto* new_slots = arena::push_array<PageCache::Slot>(arena, new_count); // zero-init = EMPTY
        for (U64 i = 0; i < cache.slot_count; i++) {
            if (cache.slots[i].key != PageCache::EMPTY) {
                U64 h = page_cache_hash(cache.slots[i].key) & (new_count - 1);
                while (new_slots[h].key != PageCache::EMPTY)
                    h = (h + 1) & (new_count - 1);
                new_slots[h] = cache.slots[i];
            }
        }
        cache.slots = new_slots;
        cache.slot_count = new_count;
    }

    static PageCache::Slot& page_cache_insert(PageCache& cache, Arena& arena, U64 key, U8* data, bool dirty) {
        if (!cache.slots || cache.count * 2 >= cache.slot_count)
            page_cache_grow(cache, arena);
        U64 h = page_cache_hash(key) & (cache.slot_count - 1);
        while (cache.slots[h].key != PageCache::EMPTY)
            h = (h + 1) & (cache.slot_count - 1);
        cache.slots[h] = {key, data, dirty};
        cache.count++;
        return cache.slots[h];
    }

    static void page_cache_reset(PageCache& cache) {
        cache = {};
    }


    static coroutine::Task<> read_header(Pager& pager) {
        co_await aio::file_read(
            *pager.file_io_ctx,
            pager.file,
            Rng1U64{
                .start=pager.base_offset,
                .end  =pager.base_offset + sizeof(pager.header),
            },
            &pager.header
        );
    }

    coroutine::Task<Header> create(aio::FileIOContext& file_io_ctx, os::Handle file, U64 page_size, U64 base_offset) {
        assert_true(sizeof(Header) <= page_size, "header does not fit in root page");
        assert_true(page_size % sizeof(U64) == 0, "invalid page size alignment");

        Header header{
            .magic=HEADER_MAGIC,
            .version=HEADER_CURRENT_VERSION,
            .page_size=page_size,
            .page_count=1, // header page
            .root_page=MAX_U64,
        };

        os::file_resize_zero(file, base_offset + header.page_count*header.page_size);
        co_await aio::file_write(file_io_ctx, file, Rng1U64{ .start=base_offset, .end=base_offset + sizeof(header) }, &header);

        co_return header;
    }

    // Flush dirty pages directly to the main file (no-WAL fallback used in destroy).
    static coroutine::Task<> flush_direct(Pager& pager) {
        aio::FileIOContext& ctx = *pager.file_io_ctx;
        for (U64 i = 0; i < pager.page_cache.slot_count; i++) {
            auto& slot = pager.page_cache.slots[i];
            if (slot.key == PageCache::EMPTY || !slot.dirty) continue;
            U64 idx = slot.key;
            if (idx < pager.header.page_count) {
                U64 offset = pager.base_offset + pager.header.page_size * idx;
                co_await aio::file_write(
                    ctx, pager.file,
                    Rng1U64{.start=offset, .end=offset + pager.header.page_size},
                    slot.data
                );
            }
        }
        if (pager.header_in_write_set) {
            co_await aio::file_write(
                ctx, pager.file,
                Rng1U64{.start=pager.base_offset, .end=pager.base_offset + sizeof(pager.header)},
                &pager.header
            );
            pager.header_in_write_set = false;
        }
        os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * pager.header.page_size);
        co_await aio::file_sync(ctx, pager.file);
        page_cache_reset(pager.page_cache);
        arena::clear(pager.page_cache_arena);
    }

    static coroutine::Task<> maybe_recover_wal(Pager& pager) {
        if (!pager.wal) co_return;
        if (!co_await wal::try_load(pager.wal, *pager.file_io_ctx, pager.header.page_size)) co_return;
        if (pager.wal.header.frame_count == 0) co_return;

        U64 page_size = pager.wal.header.page_size;
        {
            threads::Scope scratch = threads::scratch();
            U8* buf = arena::push_array<U8>(*scratch.arena, max(page_size, static_cast<U64>(sizeof(Header))));
            for (U64 i = 0; i < pager.wal.header.frame_count; i++) {
                U64 offset = sizeof(wal::Header) + i * (sizeof(wal::Frame) + page_size);

                wal::Frame frame{};
                co_await aio::file_read(*pager.file_io_ctx, pager.wal.file, Rng1U64{.start=offset, .end=offset+sizeof(wal::Frame)}, &frame);

                U64 data_size = frame.page_idx == MAX_U64 ? sizeof(Header) : page_size;
                co_await aio::file_read(*pager.file_io_ctx, pager.wal.file, Rng1U64{.start=offset + sizeof(wal::Frame), .end=offset + sizeof(wal::Frame) + data_size}, buf);

                if (frame.page_idx == MAX_U64) {
                    co_await aio::file_write(*pager.file_io_ctx, pager.file, Rng1U64{.start=pager.base_offset, .end=pager.base_offset + sizeof(Header)}, buf);
                } else {
                    U64 db_start = pager.base_offset + page_size * frame.page_idx;
                    co_await aio::file_write(*pager.file_io_ctx, pager.file, Rng1U64{.start=db_start, .end=db_start + page_size}, buf);
                }
            }
        }

        co_await aio::file_sync(*pager.file_io_ctx, pager.file);

        pager.wal.header.frame_count = 0;
        co_await aio::file_write(
            *pager.file_io_ctx,
            pager.wal.file,
            Rng1U64{
                .start=offsetof(wal::Header, frame_count),
                .end  =offsetof(wal::Header, frame_count) + sizeof(U64)
            },
            &pager.wal.header.frame_count
        );
        co_await aio::file_sync(*pager.file_io_ctx, pager.wal.file);

        co_await read_header(pager);
        // truncate file to the page count recorded in the recovered header
        os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * pager.header.page_size);
    }

    Pager::Pager(aio::FileIOContext* file_io_ctx, os::Handle file, const Header& header, U64 checkpoint_interval, U64 base_offset)
       : file_io_ctx(file_io_ctx), file(file), base_offset(base_offset), header(header), checkpoint_interval(checkpoint_interval) {
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, U64 base_offset) {
        p.file_io_ctx = file_io_ctx;
        p.file = file;
        p.base_offset = base_offset;
        co_await read_header(p);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, U64 base_offset) {
        p.file_io_ctx = file_io_ctx;
        p.file = file;
        p.base_offset = base_offset;
        p.wal = Wal{wal_file};
        co_await read_header(p);
        co_await maybe_recover_wal(p);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset) {
        wal::Header wal_hdr = co_await wal::create(*file_io_ctx, wal_file, header.page_size);
        p.file_io_ctx = file_io_ctx;
        p.file = file;
        p.base_offset = base_offset;
        p.header = header;
        p.wal = Wal{wal_file, wal_hdr};
    }

    coroutine::Task<> destroy(Pager& p) {
        assert_true(p.file_io_ctx != nullptr, "destroy requires file_io_ctx");
        if (p.transaction_active)
            rollback_transaction(p);
        else if (!p.wal)
            co_await flush_direct(p);
        p.file_io_ctx = nullptr;
    }

    Pager::Pager(Pager&& other):
        file_io_ctx(other.file_io_ctx),
        file(other.file),
        base_offset(other.base_offset),
        header(other.header),
        saved_header(other.saved_header),
        header_in_write_set(other.header_in_write_set),
        transaction_active(other.transaction_active),
        checkpoint_interval(other.checkpoint_interval),
        wal_transaction_count(other.wal_transaction_count),
        page_cache_arena(move(other.page_cache_arena)),
        page_cache(other.page_cache),
        wal(move(other.wal))
    {
        other.file = os::zero_handle();
        other.file_io_ctx = nullptr;
        other.page_cache = {};
        other.transaction_active = false;
    }

    Pager::~Pager() {
        assert_true(file_io_ctx == nullptr, "pager destroyed without destroy()");
    }

    Pager& Pager::operator=(Pager&& other) {
        if (this == &other) {
            return *this;
        }

        assert_true(file_io_ctx == nullptr, "pager overwritten without destroy()");

        file = other.file;
        base_offset = other.base_offset;
        file_io_ctx = other.file_io_ctx;
        header = other.header;
        saved_header = other.saved_header;
        header_in_write_set = other.header_in_write_set;
        transaction_active = other.transaction_active;
        checkpoint_interval = other.checkpoint_interval;
        wal_transaction_count = other.wal_transaction_count;
        page_cache_arena = move(other.page_cache_arena);
        page_cache = other.page_cache;
        wal = move(other.wal);

        other.file = os::zero_handle();
        other.file_io_ctx = nullptr;
        other.page_cache = {};
        other.transaction_active = false;

        return *this;
    }

    void begin_transaction(Pager& pager) {
        assert_true(!pager.transaction_active, "transaction already active");
        pager.saved_header = pager.header;
        pager.transaction_active = true;
    }

    coroutine::Task<> commit_transaction(Pager& pager) {
        assert_true(pager.transaction_active, "no active transaction");

        if (!pager.wal) {
            co_await flush_direct(pager);
            pager.transaction_active = false;
            co_return;
        }

        aio::FileIOContext& ctx = *pager.file_io_ctx;

        for (U64 i = 0; i < pager.page_cache.slot_count; i++) {
            auto& slot = pager.page_cache.slots[i];
            if (slot.key == PageCache::EMPTY || !slot.dirty) continue;
            U64 idx = slot.key;
            if (idx < pager.header.page_count) {
                U64 frame_idx = co_await wal::append_frame(pager.wal, ctx, idx, slot.data);
                insert(pager.wal.wal_index, idx, frame_idx);
            }
        }
        if (pager.header_in_write_set) {
            U64 frame_idx = co_await wal::append_frame(pager.wal, ctx, MAX_U64, reinterpret_cast<U8*>(&pager.header));
            insert(pager.wal.wal_index, MAX_U64, frame_idx);
            pager.header_in_write_set = false;
        }

        if (pager.wal.header.frame_count > 0) {
            co_await wal::commit(pager.wal, ctx);
            pager.wal_transaction_count++;
            if (pager.wal_transaction_count >= pager.checkpoint_interval) {
                co_await checkpoint(pager);
            }
        }

        page_cache_reset(pager.page_cache);
        arena::clear(pager.page_cache_arena);
        pager.transaction_active = false;
    }

    void rollback_transaction(Pager& pager) {
        plugin::message(pager_producer, plugin::Level::Error, "pager: transaction rolled back");
        pager.header = pager.saved_header;
        pager.header_in_write_set = false;
        page_cache_reset(pager.page_cache);
        arena::clear(pager.page_cache_arena);
        if (pager.wal) {
            pager.wal.header.frame_count = 0;
            clear(pager.wal.wal_index);
        }
        pager.transaction_active = false;
    }

    coroutine::Task<> checkpoint(Pager& pager) { ZoneScopedN("pager::checkpoint");
        assert_true(pager.wal, "checkpoint requires WAL");
        assert_true(pager.file_io_ctx != nullptr, "checkpoint requires file_io_ctx");

        aio::FileIOContext& ctx = *pager.file_io_ctx;
        U64 page_size = pager.wal.header.page_size;

        threads::Scope scratch = threads::scratch();
        U8* buf = arena::push_array<U8>(*scratch.arena, max(page_size, static_cast<U64>(sizeof(Header))));

        for (auto& pair : pager.wal.wal_index) {
            U64 page_idx  = pair.first;
            U64 frame_idx = pair.second;
            if (page_idx == MAX_U64) {
                co_await aio::file_write(
                    ctx, pager.file,
                    Rng1U64{pager.base_offset, pager.base_offset + sizeof(Header)},
                    &pager.header
                );
            } else {
                auto* slot = page_cache_find(pager.page_cache, page_idx);
                if (slot) {
                    U64 db_start = pager.base_offset + page_size * page_idx;
                    co_await aio::file_write(
                        ctx, pager.file,
                        Rng1U64{.start=db_start, .end=db_start + page_size},
                        slot->data
                    );
                } else {
                    wal::Frame frame{};
                    co_await wal::read_frame(pager.wal, ctx, frame_idx, frame, buf);
                    U64 db_start = pager.base_offset + page_size * page_idx;
                    co_await aio::file_write(
                        ctx, pager.file,
                        Rng1U64{.start=db_start, .end=db_start + page_size},
                        buf
                    );
                }
            }
        }

        os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * page_size);
        co_await aio::file_sync(ctx, pager.file);
        co_await wal::reset(pager.wal, ctx);
        clear(pager.wal.wal_index);
        pager.wal_transaction_count = 0;
    }

    coroutine::Task<> set_root(Pager& pager, U64 page) {
        assert_true(pager.transaction_active, "no transaction active");
        pager.header.root_page = page;
        pager.header_in_write_set = true;
        co_return;
    }

    coroutine::Task<const U8*> rpage(Pager& pager, U64 idx) {
        assert_true(pager.transaction_active, "rpage requires an active transaction");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rpage requires file_io_ctx");

        if (auto* slot = page_cache_find(pager.page_cache, idx))
            co_return slot->data;

        U8* data = arena::push_array_no_zero<U8>(pager.page_cache_arena, pager.header.page_size);
        bool loaded = false;
        if (pager.wal) {
            if (U64* frame_idx_ptr = find(pager.wal.wal_index, idx)) {
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, *pager.file_io_ctx, *frame_idx_ptr, frame, data);
                loaded = true;
            }
        }
        if (!loaded) {
            auto offset = pager.base_offset + pager.header.page_size * idx;
            co_await aio::file_read(
                *pager.file_io_ctx, pager.file,
                Rng1U64{.start=offset, .end=offset + pager.header.page_size},
                data
            );
        }
        page_cache_insert(pager.page_cache, pager.page_cache_arena, idx, data, false);
        co_return data;
    }

    coroutine::Task<U8*> rwpage(Pager& pager, U64 idx) {
        assert_true(pager.transaction_active, "no transaction active");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rwpage requires file_io_ctx");

        if (auto* slot = page_cache_find(pager.page_cache, idx)) {
            slot->dirty = true;
            co_return slot->data;
        }

        U8* data = arena::push_array_no_zero<U8>(pager.page_cache_arena, pager.header.page_size);
        bool loaded = false;
        if (pager.wal) {
            if (U64* frame_idx_ptr = find(pager.wal.wal_index, idx)) {
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, *pager.file_io_ctx, *frame_idx_ptr, frame, data);
                loaded = true;
            }
        }
        if (!loaded) {
            auto offset = pager.base_offset + pager.header.page_size * idx;
            co_await aio::file_read(
                *pager.file_io_ctx, pager.file,
                Rng1U64{.start=offset, .end=offset + pager.header.page_size},
                data
            );
        }
        page_cache_insert(pager.page_cache, pager.page_cache_arena, idx, data, true);
        co_return data;
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
        // file is truncated at checkpoint (WAL) or flush_direct (no-WAL) using the committed page_count
        pager.header_in_write_set = true;
    }
}
