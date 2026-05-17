export module plexdb.pager;

import plexdb.pager.wal;
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
        aio::FileIOContext* file_io_ctx = nullptr;

        static constexpr U64 alignment = sizeof(U64);
        os::Handle file = os::zero_handle();
        U64 base_offset = 0;

        Header header = {};
        Header saved_header = {};
        bool header_in_write_set = false;
        bool transaction_active  = false;

        // Checkpoint every N committed transactions; 0 checkpoints after every commit.
        U64 checkpoint_interval     = 0;
        U64 wal_transaction_count   = 0;

        U64 read_cache_count    = 0;
        U8* read_cache          = nullptr;
        struct ReadCacheEntry {
            bool in_write_set;
            Deque<U64>::Node* write_set_node;
            U64 idx;
        };

        Arena write_arena;
        Deque<U64> write_set;

        // Optional WAL: when present, commit_transaction writes through WAL before checkpointing.
        Wal wal;

        Pager() = default;
        Pager(aio::FileIOContext* file_io_ctx, os::Handle file, const Header& header, U64 checkpoint_interval=2, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(Pager&& other);
        ~Pager();

        Pager& operator=(Pager&& other);

        Pager(const Pager&) = delete;
        Pager& operator=(const Pager&) = delete;

        operator bool() const { return !os::is_zero_handle(this->file); }
    };

    coroutine::Task<Header> create(aio::FileIOContext& file_io_ctx, os::Handle file, U64 page_size, U64 base_offset=0);

    // open: reads header from disk and optionally recovers WAL
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
    // open with known header: async only because it creates the WAL header on disk
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);

    // flush pending writes and mark pager as closed (destructor then only frees memory)
    coroutine::Task<> destroy(Pager& pager);

    // transaction lifecycle — WAL must be present for begin/commit/rollback/checkpoint
    void              begin_transaction(Pager& pager);
    coroutine::Task<> commit_transaction(Pager& pager);
    void              rollback_transaction(Pager& pager);
    coroutine::Task<> checkpoint(Pager& pager);

    coroutine::Task<> set_root(Pager& pager, U64 page);

    coroutine::Task<const U8*> rpage(Pager& pager, U64 idx);
    coroutine::Task<U8*> rwpage(Pager& pager, U64 idx);

    coroutine::Task<U64> new_page(Pager& pager);
    coroutine::Task<> delete_page(Pager& pager, U64 idx);
}

export namespace plexdb {
    using Pager = pager::Pager;
}
