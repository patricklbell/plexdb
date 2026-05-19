export module plexdb.pager;

import plexdb.pager.wal;
import plexdb.pager.types;

import plexdb.base;
import plexdb.os;
import plexdb.arena;
import plexdb.aio;
import plexdb.coroutine;

export namespace plexdb::pager {
    struct PageCache {
        static constexpr U64 EMPTY = 0;  // @note page 0 is always invalid
        struct Slot {
            U64 key;
            U8* data;
            bool dirty;
        };
        Slot* slots      = nullptr;  // @note arena-allocated; invalidated when arena is cleared
        U64   slot_count = 0;
        U64   count      = 0;
    };

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

        Arena page_cache_arena;
        PageCache page_cache;

        // @note optional
        Wal wal;

        Pager() = default;
        Pager(aio::FileIOContext* file_io_ctx, os::Handle file, const Header& header, U64 checkpoint_interval=2, U64 base_offset=0);
        Pager(Pager&& other);
        ~Pager();

        Pager& operator=(Pager&& other);

        Pager(const Pager&) = delete;
        Pager& operator=(const Pager&) = delete;

        operator bool() const { return !os::is_zero_handle(this->file); }
    };

    coroutine::Task<Header> create(aio::FileIOContext& file_io_ctx, os::Handle file, U64 page_size, U64 base_offset=0);

    // open: reads header from disk and optionally recovers WAL
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, U64 base_offset=0);
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, U64 base_offset=0);
    // open with known header
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset=0);

    // flush pending writes and mark pager as closed
    coroutine::Task<> destroy(Pager& pager);

    // transaction lifecycle
    void              begin_transaction(Pager& pager);
    coroutine::Task<> commit_transaction(Pager& pager);
    void              rollback_transaction(Pager& pager);
    coroutine::Task<> checkpoint(Pager& pager);

    // @note require an active transaction
    coroutine::Task<>           set_root(Pager& pager, U64 page);

    coroutine::Task<const U8*>  rpage(Pager& pager, U64 idx);
    coroutine::Task<U8*>        rwpage(Pager& pager, U64 idx);

    coroutine::Task<U64>        new_page(Pager& pager);
    coroutine::Task<>           delete_page(Pager& pager, U64 idx);
}

export namespace plexdb {
    using Pager = pager::Pager;
}
