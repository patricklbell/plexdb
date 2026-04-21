export module plexdb.pager;

export import plexdb.pager.wal;

import plexdb.base;
import plexdb.os;
import plexdb.arena;

export namespace plexdb::pager {
    constexpr U64 DEFAULT_READ_CACHE = 1000u;
    constexpr Array<U8,6> HEADER_MAGIC{'p', 'X', 'D', 'b', 1, 12};
    constexpr Array<U8,2> HEADER_CURRENT_VERSION{ 0, 1 };

    // @todo lru
    // @todo experiment with segmented LFU or CLOCK
    struct Pager {
        static constexpr U64 alignment = sizeof(U64);
        os::Handle file = os::zero_handle();
        U64 base_offset = 0;

        // @padding
        struct Header {
            Array<U8,sizeof(HEADER_MAGIC)> magic;
            Array<U8,sizeof(HEADER_CURRENT_VERSION)> version;
            U64 page_size;
            U64 page_count;
            U64 root_page;
        } header = {};
        bool header_in_write_set = false;

        // @todo
        U64 root_page = 2;

        // pager flushes writes to disk when cache read collision occurs. @profile
        // a collision requires that the page is in the write set, otherwise the
        // cache is just evicted.
        U64 read_cache_count    = 0;
        U8* read_cache          = nullptr;
        struct ReadCacheEntry {
            bool in_write_set;
            U64 idx;
        };

        Arena write_arena;
        Stack<U64> write_set;

        // Optional WAL: when present, fflush writes through WAL before checkpointing.
        Wal wal;

        Pager() = default;
        Pager(os::Handle file, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(os::Handle file, const Pager::Header& header, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        // WAL-enabled constructors: wal_file must be a valid handle.
        Pager(os::Handle file, os::Handle wal_file, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(os::Handle file, os::Handle wal_file, const Pager::Header& header, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(Pager&& other);
        ~Pager();

        Pager& operator=(Pager&& other);

        Pager(const Pager&) = delete;
        Pager& operator=(const Pager&) = delete;
    };

    Pager::Header create(os::Handle file, U64 page_size, U64 base_offset=0);

    void set_root(Pager& pager, U64 page);

    const U8* rpage(Pager& pager, U64 idx);
    U8* rwpage(Pager& pager, U64 idx);
    void fflush(Pager& pager);

    U64 new_page(Pager& pager);
    void delete_page(Pager& pager, U64 idx);
}

export namespace plexdb {
    using Pager = pager::Pager;
}