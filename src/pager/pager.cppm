export module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.arena;

export namespace plexdb::pager {
    constexpr U64 DEFAULT_READ_CACHE = 1000u;

    struct Pager {
        os::Handle file;

        U64 page_size;

        // pager flushes writes to disk when cache read collision occurs. @profile
        // a collision requires that the page is in the write set, otherwise the
        // cache is just evicted.
        U64 read_cache_count;
        U8* read_cache;
        struct ReadCacheEntry {
            U64 idx;
            bool flushed;
        };

        Arena write_arena;
        Stack<U64> write_set;

        Pager(os::Handle file, U64 page_size);
        Pager(String8 file_path, U64 page_size);
        ~Pager();

        Pager() = delete;
        Pager(const Pager&) = delete;
        Pager& operator=(const Pager&) = delete;
    };

    const U8* rpage(Pager& pager, U64 idx);
    U8* wpage(Pager& pager, U64 idx);
    U8* rwpage(Pager& pager, U64 idx);
    void fflush(Pager& pager);

}