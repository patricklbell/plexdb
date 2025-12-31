export module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.arena;

export namespace plexdb::pager {
    constexpr U64 DEFAULT_READ_CACHE = 1000u;
    constexpr U8 HEADER_MAGIC[6] = {'p', 'X', 'D', 'b', 1, 12};
    constexpr U8 HEADER_CURRENT_VERSION[2] = { 0, 1 };

    struct Pager {
        os::Handle file;
        const U64 base_offset;

        // @padding
        struct Header {
            U8 magic[sizeof(HEADER_MAGIC)];
            U8 version[sizeof(HEADER_CURRENT_VERSION)];
            U64 page_size;
            U64 page_count;
            U64 free_list_count;
        } header;

        // pager flushes writes to disk when cache read collision occurs. @profile
        // a collision requires that the page is in the write set, otherwise the
        // cache is just evicted.
        U64 read_cache_count;
        U8* read_cache;
        struct ReadCacheEntry {
            bool flushed;
            U64 idx;
        };

        Arena write_arena;
        Stack<U64> write_set;

        Pager(os::Handle file);
        Pager(os::Handle file, Pager::Header header, U64 base_offset);
        ~Pager();

        Pager() = delete;
        Pager(const Pager&) = delete;
        Pager& operator=(const Pager&) = delete;
    };

    Pager::Header create(os::Handle file, U64 page_size);

    const U8* rpage(Pager& pager, U64 idx);
    U8* wpage(Pager& pager, U64 idx);
    U8* rwpage(Pager& pager, U64 idx);
    void fflush(Pager& pager);

    U64 new_page(Pager& pager);
    void delete_page(Pager& pager, U64 idx);
}

export namespace plexdb {
    using pager::Pager;
}