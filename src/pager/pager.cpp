module plexdb.pager;

import plexdb.base;
import plexdb.os;

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
                .end=pager.base_offset+sizeof(pager.header),
            },
            &pager.header
        );
    }
    static void write_header(Pager& pager) {
        os::file_write(
            pager.file, 
            Rng1U64{
                .start=pager.base_offset,
                .end=pager.base_offset+sizeof(pager.header),
            },
            &pager.header
        );
    }

    static void read_page(Pager& pager, U8* data, U64 idx) {
        os::file_read(
            pager.file,
            Rng1U64{
                .start=pager.base_offset+pager.header.page_size*idx,
                .end=pager.base_offset+pager.header.page_size*(idx+1)
            },
            data
        );
    }
    static void write_page(Pager& pager, U8* data, U64 idx) {
        os::file_write(
            pager.file,
            Rng1U64{
                .start=pager.base_offset+pager.header.page_size*idx,
                .end=pager.base_offset+pager.header.page_size*(idx+1)
            },
            data
        );
    }

    Pager::Header create(os::Handle file, U64 page_size) {
        assert_true(sizeof(Pager::Header) < page_size, "header does not fit in root page");

        Pager::Header header{
            .page_size=page_size,
            .page_count=1, // header page
            .free_list_count=0,
        };
        // @note constepxr array type?
        os::memory_copy(header.magic, &HEADER_MAGIC, sizeof(HEADER_MAGIC));
        os::memory_copy(header.version, &HEADER_CURRENT_VERSION, sizeof(HEADER_CURRENT_VERSION));

        U64 base_offset = os::file_get_offset(file);
        os::file_write(file, Rng1U64{ .start=base_offset, .end=base_offset+sizeof(header) }, &header);

        return header;
    }

    Pager::Pager(os::Handle file)
      : file(file), base_offset(os::file_get_offset(file)) {
        read_header(*this);
        this->read_cache = os::allocate(get_cache_size(*this)*this->read_cache_count);
    }
    Pager::Pager(os::Handle file, Pager::Header header, U64 base_offset)
       : header(header), file(file), base_offset(base_offset) {
        this->read_cache = os::allocate(get_cache_size(*this)*this->read_cache_count);
    }
    Pager::~Pager() {
        os::deallocate(this->read_cache);
        os::file_close(this->file);
    }

    const U8* rpage(Pager& pager, U64 idx) {
        auto entry = get_cache_entry(pager, idx);
        if (entry->idx == idx)
            return get_cache_data(pager, idx);
        if (!entry->flushed)
            fflush(pager);

        auto data = get_cache_data(pager, idx);
        read_page(pager, data, idx);
        entry->idx = idx;
        return data;
    }

    U8* wpage(Pager& pager, U64 idx) {
        auto entry = get_cache_entry(pager, idx);
        if (entry->idx != idx && !entry->flushed)
            fflush(pager);
        if (entry->flushed)
            push_front(pager.write_arena, pager.write_set, idx);

        entry->idx = idx;
        entry->flushed = false;
        return get_cache_data(pager, idx);
    }

    U8* rwpage(Pager& pager, U64 idx) {
        auto entry = get_cache_entry(pager, idx);
        if (entry->idx != idx && !entry->flushed)
            fflush(pager);
        if (entry->flushed)
            push_front(pager.write_arena, pager.write_set, idx);
        if (entry->idx != idx)
            read_page(pager, get_cache_data(pager, idx), idx);

        entry->idx = idx;
        entry->flushed = false;
        return get_cache_data(pager, idx);
    }

    void fflush(Pager& pager) {
        for (auto& idx : pager.write_set) {
            auto entry = get_cache_entry(pager, idx);
            write_page(pager, get_cache_data(pager, idx), idx);
            entry->flushed = true;
        }
        clear(pager.write_set);
        arena::clear(pager.write_arena);
    }

    U64 new_page(Pager& pager) {
        // @todo read free list
        U64 tail_page = pager.header.page_count;
        pager.header.page_count++;
        write_header(pager);
        return tail_page;
    }

    void delete_page(Pager& pager, U64 idx) {
        // @todo add to free list
    }
}