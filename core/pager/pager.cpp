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

    static U64 get_cache_size(Pager& pager) {
        return pager.header.page_size + sizeof(Pager::ReadCacheEntry);
    }
    static Pager::ReadCacheEntry* get_cache_entry(Pager& pager, U64 idx) {
        static_assert(sizeof(*pager.read_cache) == 1, "ptr arithmetic");
        U64 cache_idx = idx % pager.read_cache_count;
        return reinterpret_cast<Pager::ReadCacheEntry*>(
            pager.read_cache + cache_idx*get_cache_size(pager)
        );
    }
    static U8* get_cache_data(Pager& pager, U64 idx) {
        static_assert(sizeof(*pager.read_cache) == 1, "ptr arithmetic");
        U64 cache_idx = idx % pager.read_cache_count;
        return pager.read_cache + cache_idx*get_cache_size(pager) + sizeof(Pager::ReadCacheEntry);
    }
    static void init_read_cache(Pager& pager) {
        pager.read_cache = os::allocate_zero(get_cache_size(pager)*pager.read_cache_count);
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

    // Spill one dirty cache slot to the WAL without committing.
    // Removes the slot from write_set and marks it clean so the slot can be reused.
    static coroutine::Task<> spill_to_wal(Pager& pager, Pager::ReadCacheEntry* entry) {
        assert_true(pager.wal, "spill requires WAL");
        U64 page_idx = entry->idx;
        U64 frame_idx = co_await wal::append_frame(pager.wal, *pager.file_io_ctx, page_idx, get_cache_data(pager, page_idx));
        insert(pager.wal.wal_index, page_idx, frame_idx);
        remove(pager.write_set, entry->write_set_node);
        entry->in_write_set = false;
        entry->write_set_node = nullptr;
    }

    // Flush dirty pages directly to the main file (no-WAL fallback used in destroy).
    static coroutine::Task<> flush_direct(Pager& pager) {
        aio::FileIOContext& ctx = *pager.file_io_ctx;
        for (auto& idx : pager.write_set) {
            auto entry = get_cache_entry(pager, idx);
            if (idx < pager.header.page_count) {
                U64 offset = pager.base_offset + pager.header.page_size * idx;
                co_await aio::file_write(
                    ctx, pager.file,
                    Rng1U64{.start=offset, .end=offset + pager.header.page_size},
                    get_cache_data(pager, idx)
                );
            }
            entry->in_write_set = false;
            entry->write_set_node = nullptr;
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
        clear(pager.write_set);
        arena::clear(pager.write_arena);
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

    Pager::Pager(aio::FileIOContext* file_io_ctx, os::Handle file, const Header& header, U64 checkpoint_interval, U64 base_offset, U64 read_cache)
       : file_io_ctx(file_io_ctx), file(file), base_offset(base_offset), header(header), checkpoint_interval(checkpoint_interval), read_cache_count(read_cache) {
        init_read_cache(*this);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, U64 base_offset, U64 read_cache) {
        p.file_io_ctx = file_io_ctx;
        p.file = file;
        p.base_offset = base_offset;
        p.read_cache_count = read_cache;
        co_await read_header(p);
        init_read_cache(p);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, U64 base_offset, U64 read_cache) {
        p.file_io_ctx = file_io_ctx;
        p.file = file;
        p.base_offset = base_offset;
        p.read_cache_count = read_cache;
        p.wal = Wal{wal_file};
        co_await read_header(p);
        co_await maybe_recover_wal(p);
        init_read_cache(p);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset, U64 read_cache) {
        wal::Header wal_hdr = co_await wal::create(*file_io_ctx, wal_file, header.page_size);
        p.file_io_ctx = file_io_ctx;
        p.file = file;
        p.base_offset = base_offset;
        p.header = header;
        p.read_cache_count = read_cache;
        init_read_cache(p);
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
        read_cache_count(other.read_cache_count),
        read_cache(other.read_cache),
        write_arena(move(other.write_arena)),
        write_set(other.write_set),
        wal(move(other.wal))
    {
        other.file = os::zero_handle();
        other.file_io_ctx = nullptr;
        other.read_cache = nullptr;
        other.write_set = {};
        other.transaction_active = false;
    }

    Pager::~Pager() {
        assert_true(file_io_ctx == nullptr, "pager destroyed without destroy()");
        os::deallocate(this->read_cache);
    }

    Pager& Pager::operator=(Pager&& other) {
        if (this == &other) {
            return *this;
        }

        assert_true(file_io_ctx == nullptr, "pager overwritten without destroy()");
        os::deallocate(this->read_cache);

        file = other.file;
        base_offset = other.base_offset;
        file_io_ctx = other.file_io_ctx;
        header = other.header;
        saved_header = other.saved_header;
        header_in_write_set = other.header_in_write_set;
        transaction_active = other.transaction_active;
        checkpoint_interval = other.checkpoint_interval;
        wal_transaction_count = other.wal_transaction_count;
        read_cache_count = other.read_cache_count;
        read_cache = other.read_cache;
        write_arena = move(other.write_arena);
        write_set = other.write_set;
        wal = move(other.wal);

        other.file = os::zero_handle();
        other.file_io_ctx = nullptr;
        other.read_cache = nullptr;
        other.write_set = {};
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

        for (auto& idx : pager.write_set) {
            auto entry = get_cache_entry(pager, idx);
            if (idx < pager.header.page_count) {
                U64 frame_idx = co_await wal::append_frame(pager.wal, ctx, idx, get_cache_data(pager, idx));
                insert(pager.wal.wal_index, idx, frame_idx);
            }
            entry->in_write_set = false;
            entry->write_set_node = nullptr;
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

        clear(pager.write_set);
        arena::clear(pager.write_arena);
        pager.transaction_active = false;
    }

    void rollback_transaction(Pager& pager) {
        plugin::message(pager_producer, plugin::Level::Error, "pager: transaction rolled back");

        for (auto& idx : pager.write_set) {
            auto entry = get_cache_entry(pager, idx);
            entry->idx = 0;
            entry->in_write_set = false;
            entry->write_set_node = nullptr;
        }
        pager.header = pager.saved_header;
        pager.header_in_write_set = false;
        clear(pager.write_set);
        arena::clear(pager.write_arena);
        if (pager.wal) {
            // orphan any mid-transaction WAL spills (disk frame_count is still 0)
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
                auto entry = get_cache_entry(pager, page_idx);
                if (entry->idx == page_idx) {
                    U64 db_start = pager.base_offset + page_size * page_idx;
                    co_await aio::file_write(
                        ctx, pager.file,
                        Rng1U64{.start=db_start, .end=db_start + page_size},
                        get_cache_data(pager, page_idx)
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
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rpage requires file_io_ctx");

        auto entry = get_cache_entry(pager, idx);
        if (entry->idx == idx)
            co_return get_cache_data(pager, idx);

        if (entry->in_write_set) {
            if (pager.wal) co_await spill_to_wal(pager, entry);
            else           co_await flush_direct(pager);
        }

        auto data = get_cache_data(pager, idx);

        if (pager.wal) {
            if (U64* frame_idx_ptr = find(pager.wal.wal_index, idx)) {
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, *pager.file_io_ctx, *frame_idx_ptr, frame, data);
                entry->idx = idx;
                entry->in_write_set = false;
                entry->write_set_node = nullptr;
                co_return data;
            }
        }

        auto offset = pager.base_offset + pager.header.page_size * idx;
        co_await aio::file_read(
            *pager.file_io_ctx, pager.file,
            Rng1U64{.start=offset, .end=offset + pager.header.page_size},
            data
        );
        entry->idx = idx;
        co_return data;
    }

    coroutine::Task<U8*> rwpage(Pager& pager, U64 idx) {
        assert_true(pager.transaction_active, "no transaction active");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rwpage requires file_io_ctx");

        auto entry = get_cache_entry(pager, idx);

        // fast path: already cached and dirty
        if (entry->idx == idx && entry->in_write_set)
            co_return get_cache_data(pager, idx);

        // collision with a dirty slot — spill it before evicting
        if (entry->in_write_set) {
            if (pager.wal) co_await spill_to_wal(pager, entry);
            else           co_await flush_direct(pager);
        }

        // load page if not already in cache
        if (entry->idx != idx) {
            auto data = get_cache_data(pager, idx);
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
        }

        push_front(pager.write_arena, pager.write_set, idx);
        entry->write_set_node = pager.write_set.head;
        entry->idx = idx;
        entry->in_write_set = true;
        co_return get_cache_data(pager, idx);
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
