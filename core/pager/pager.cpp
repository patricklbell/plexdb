module;
#include "macros.h"

module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.threads;
import plexdb.arena;

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
    static void read_page(Pager& pager, U8* data, U64 idx) {
        os::file_read(
            pager.file,
            Rng1U64{
                .start=pager.base_offset + pager.header.page_size*(idx + 0),
                .end  =pager.base_offset + pager.header.page_size*(idx + 1)
            },
            data
        );
    }
    static void write_page(Pager& pager, U8* data, U64 idx) {
        os::file_write(
            pager.file,
            Rng1U64{
                .start=pager.base_offset + pager.header.page_size*(idx + 0),
                .end  =pager.base_offset + pager.header.page_size*(idx + 1)
            },
            data
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

    static void maybe_recover_wal(Pager& pager) {
        if (!pager.wal) return;
        if (!wal_load(pager.wal, pager.header.page_size)) return;
        if (!wal_has_committed(pager.wal)) return;

        wal_checkpoint(pager.wal, pager.file, pager.base_offset);
        wal_reset(pager.wal);
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
        header(other.header),
        header_in_write_set(other.header_in_write_set),
        read_cache_count(other.read_cache_count),
        read_cache(other.read_cache),
        write_arena(move(other.write_arena)),
        write_set(move(other.write_set)),
        wal(move(other.wal))
    {
        other.file = os::zero_handle();
        other.read_cache = nullptr;
    }
    Pager::~Pager() {
        if (!os::is_zero_handle(this->file)) {
            fflush(*this);
            os::file_sync(this->file);
        }

        os::deallocate(this->read_cache);
    }

    Pager& Pager::operator=(Pager&& other) {
        if (this == &other) {
            return *this;
        }

        if (!os::is_zero_handle(this->file)) {
            fflush(*this);
            os::file_sync(this->file);
        }

        os::deallocate(this->read_cache);

        file = other.file;
        base_offset = other.base_offset;
        header = other.header;
        header_in_write_set = other.header_in_write_set;
        read_cache_count = other.read_cache_count;
        read_cache = other.read_cache;
        write_arena = move(other.write_arena);
        write_set = move(other.write_set);
        wal = move(other.wal);

        other.file = os::zero_handle();
        other.read_cache = nullptr;

        return *this;
    }

    void set_root(Pager& pager, U64 page) {
        pager.header.root_page = page;
        pager.header_in_write_set = true;
    }

    const U8* rpage(Pager& pager, U64 idx) {
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");

        auto entry = get_cache_entry(pager, idx);
        if (entry->idx == idx)
            return get_cache_data(pager, idx);
        if (entry->in_write_set)
            fflush(pager);

        auto data = get_cache_data(pager, idx);
        read_page(pager, data, idx);
        entry->idx = idx;
        return data;
    }

    U8* rwpage(Pager& pager, U64 idx) {
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");

        auto entry = get_cache_entry(pager, idx);

        // clear cached write @todo this overwrites memory which may be held in a transaction
        if (entry->idx != 0 && entry->idx != idx && entry->in_write_set)
            fflush(pager);
        if (!entry->in_write_set)
            push_front(pager.write_arena, pager.write_set, idx);
        if (entry->idx != idx)
            read_page(pager, get_cache_data(pager, idx), idx);

        entry->idx = idx;
        entry->in_write_set = true;
        return get_cache_data(pager, idx);
    }

    void fflush(Pager& pager) {
        if (pager.wal) {
            assert_true(pager.wal.header.page_size != 0, "invalid wal page size");

            U64 frames_appended = 0;
            for (auto& idx : pager.write_set) {
                auto entry = get_cache_entry(pager, idx);
                assert_true(entry->idx != 0, "header page found in write set");
                if (idx < pager.header.page_count) {
                    wal_append_frame(pager.wal, idx, get_cache_data(pager, idx));
                    frames_appended++;
                }
                entry->in_write_set = false;
            }
            if (pager.header_in_write_set) {
                wal_append_frame(pager.wal, MAX_U64, reinterpret_cast<U8*>(&pager.header));
                pager.header_in_write_set = false;
                frames_appended++;
            }

            if (frames_appended > 0) {
                wal_commit(pager.wal);
                wal_checkpoint(pager.wal, pager.file, pager.base_offset);
                wal_reset(pager.wal);
            }
        } else {
            for (auto& idx : pager.write_set) {
                auto entry = get_cache_entry(pager, idx);
                assert_true(entry->idx != 0, "header page found in write set");
                if (idx < pager.header.page_count) {
                    write_page(pager, get_cache_data(pager, idx), idx);
                }
                entry->in_write_set = false;
            }
            if (pager.header_in_write_set) {
                write_header(pager);
                pager.header_in_write_set = false;
            }
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
    inline static const U64* rbitset(Pager& pager, U64 idx) {
        return reinterpret_cast<const U64*>(rpage(pager, idx));
    }
    inline static U64* rwbitset(Pager& pager, U64 idx) {
        return reinterpret_cast<U64*>(rwpage(pager, idx));
    }

    U64 new_page(Pager& pager) {
        const U64 ENTRIES_PER_BITSET = get_bitset_entry_count(pager);
        const U64 BITSET_PAGE_STRIDE = ENTRIES_PER_BITSET*get_pages_per_entry() + 1;
        auto& h = pager.header;

        // scan bitsets for free page @perf
        for (U64 bitset_page = 1; bitset_page < h.page_count; bitset_page += BITSET_PAGE_STRIDE) {
            U64* bitset = rwbitset(pager, bitset_page);

            // @perf
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
                    return free_page;
                }
            }
        }

        // add new bitset
        assert_true((h.page_count - 1) % BITSET_PAGE_STRIDE == 0, "bitset correctly aligned");

        U64 bitset_page = h.page_count++;
        U64 free_page = h.page_count++;
        pager.header_in_write_set = true;
        os::file_resize_zero(pager.file, pager.base_offset + h.page_count*h.page_size);

        U64* bitset = rwbitset(pager, bitset_page);
        bitset[0] |= 1_u64;

        return free_page;
    }

    void delete_page(Pager& pager, U64 idx) {
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        const U64 PAGES_PER_BITSET = get_bitset_entry_count(pager)*get_pages_per_entry() + 1;
        auto& h = pager.header;

        U64 bitset_page = ((idx - 1)/PAGES_PER_BITSET)*PAGES_PER_BITSET + 1;
        assert_true(idx != bitset_page, "invalid delete page");
        U64* bitset = rwbitset(pager, bitset_page);

        // mark the page as free in bitset
        U64 bitset_idx = idx - bitset_page - 1;
        U64 entry_idx  = bitset_idx / get_pages_per_entry();
        U64 bit_idx    = bitset_idx % get_pages_per_entry();
        assert_true(bitset[entry_idx] & 1_u64 << bit_idx, "trying to delete page marked as deleted");
        bitset[entry_idx] &= ~(1_u64 << bit_idx);

        // early exit trimming @note not fault tolerant
        if (idx != h.page_count - 1)
            return;

        // trim dead pages
        const U64* bitset_r = bitset;
        PLEXDB_DEBUG_X(U64 old_page_count = h.page_count);
        while (true) {
            for (;; entry_idx--) {
                // entry is not completely free, find pages we can trim
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

            // if the bitset was not completely free or there are no bitsets left, stop
            if (h.page_count != bitset_page || h.page_count <= PAGES_PER_BITSET)
                break;
            // otherwise, try to keep trimming pages from the previous bitset
            bitset_page -= PAGES_PER_BITSET;
            bitset_r = rbitset(pager, bitset_page);
            entry_idx = get_bitset_entry_count(pager) - 1;
        }
        PLEXDB_DEBUG_X(assert_true(h.page_count < old_page_count, "trimming actually trimmed"));
        os::file_resize_zero(pager.file, pager.base_offset + h.page_count*h.page_size);
        pager.header_in_write_set = true;
    }
}
