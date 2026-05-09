export module plexdb.pager;

export import plexdb.pager.wal;
import plexdb.pager.types;

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.aio;
import plexdb.coroutine;

export namespace plexdb::pager {
    constexpr U64 DEFAULT_READ_CACHE = 1000u;

    // @todo lru
    // @todo experiment with segmented LFU or CLOCK
    struct Pager {
        static constexpr U64 alignment = sizeof(U64);
        os::Handle file = os::zero_handle();
        U64 base_offset = 0;
        
        aio::FileIOContext* file_io_ctx = nullptr;

        Header header = {};
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
        Pager(os::Handle file, const Header& header, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(os::Handle file, os::Handle wal_file, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(Pager&& other);
        ~Pager();

        Pager& operator=(Pager&& other);

        Pager(const Pager&) = delete;
        Pager& operator=(const Pager&) = delete;

        operator bool() const { return !os::is_zero_handle(this->file); }
    };

    Header create(os::Handle file, U64 page_size, U64 base_offset=0);

    void set_root(Pager& pager, U64 page);

    coroutine::Task<const U8*> rpage(Pager& pager, U64 idx);
    coroutine::Task<U8*> rwpage(Pager& pager, U64 idx);
    coroutine::Task<> fflush(Pager& pager);

    coroutine::Task<U64> new_page(Pager& pager);
    coroutine::Task<> delete_page(Pager& pager, U64 idx);
}

export namespace plexdb {
    using Pager = pager::Pager;
}
