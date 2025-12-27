module plexdb.pager;

import plexdb.base;
import plexdb.os;

namespace plexdb::pager {
    inline static U64 get_cache_size(Pager& pager) {
        return pager.page_size + sizeof(Pager::ReadCacheEntry);
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

    Pager::Pager(String8 file_path, U64 page_size)
      : Pager(os::file_open(file_path, os::AccessFlags(os::READ|os::WRITE)), page_size) {}
    Pager::Pager(os::Handle file, U64 page_size)
      : file(file), write_arena(), page_size(page_size), read_cache_count(DEFAULT_READ_CACHE) {
        this->read_cache = os::allocate(get_cache_size(*this)*this->read_cache_count);
    }
    Pager::~Pager() {
        os::deallocate(this->read_cache);
        os::file_close(this->file);
    }

    static void read_page_data(Pager& pager, U8* data, U64 idx) {
        os::file_read(
            pager.file,
            Rng1U64{.start=pager.page_size*idx, .end=pager.page_size*(idx+1)},
            data
        );
    }
    static void write_page_data(Pager& pager, U8* data, U64 idx) {
        os::file_write(
            pager.file,
            Rng1U64{.start=pager.page_size*idx, .end=pager.page_size*(idx+1)},
            data
        );
    }

    const U8* rpage(Pager& pager, U64 idx) {
        auto entry = get_cache_entry(pager, idx);
        if (entry->idx == idx)
            return get_cache_data(pager, idx);
        if (!entry->flushed)
            fflush(pager);

        auto data = get_cache_data(pager, idx);
        read_page_data(pager, data, idx);
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
            read_page_data(pager, get_cache_data(pager, idx), idx);

        entry->idx = idx;
        entry->flushed = false;
        return get_cache_data(pager, idx);
    }

    void fflush(Pager& pager) {
        for (auto& idx : pager.write_set) {
            auto entry = get_cache_entry(pager, idx);
            write_page_data(pager, get_cache_data(pager, idx), idx);
            entry->flushed = true;
        }
        clear(pager.write_set);
        arena::clear(pager.write_arena);
    }  
}