module;
#include <coroutine>
export module plexdb.pager;

import plexdb.pager.wal;
import plexdb.pager.types;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.coroutine;

export namespace plexdb::pager {
    struct PageCacheEntry {
        UniquePtr<U8> data;
        bool          dirty = false;
    };

    struct Pager {
        aio::FileIOContext* file_io_ctx = nullptr;

        static constexpr U64 alignment   = sizeof(U64);
        os::Handle           file        = os::zero_handle();
        U64                  base_offset = 0;

        Header                         header              = {};
        Header                         saved_header        = {};
        bool                           header_in_write_set = false;
        bool                           transaction_active  = false;
        Deque<std::coroutine_handle<>> tx_waiters;

        U64 checkpoint_interval   = 0;
        U64 wal_transaction_count = 0;

        DynamicMap<U64, PageCacheEntry> page_cache;

        // @note optional
        Wal wal;

        Pager() = default;
        Pager(aio::FileIOContext* file_io_ctx, os::Handle file, const Header& header, U64 checkpoint_interval = 2, U64 base_offset = 0);
        Pager(Pager&& other);
        ~Pager();

        Pager& operator=(Pager&& other);

        Pager(const Pager&)            = delete;
        Pager& operator=(const Pager&) = delete;

        operator bool() const {
            return !os::is_zero_handle(this->file);
        }
    };

    coroutine::Task<Header> create(aio::FileIOContext& file_io_ctx, os::Handle file, U64 page_size, U64 base_offset = 0);

    // open: reads header from disk and optionally recovers WAL
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, U64 base_offset = 0);
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, U64 base_offset = 0);
    // open with known header
    coroutine::Task<> init(Pager& pager, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset = 0);

    // flush pending writes and mark pager as closed
    coroutine::Task<> destroy(Pager& pager);

    coroutine::Task<> checkpoint(Pager& pager);

    // @note require an active transaction
    coroutine::Task<> set_root(Pager& pager, U64 page);

    coroutine::Task<const U8*> rpage(Pager& pager, U64 idx);
    coroutine::Task<U8*>       rwpage(Pager& pager, U64 idx);

    coroutine::Task<U64> new_page(Pager& pager);
    coroutine::Task<>    delete_page(Pager& pager, U64 idx);

    // ========================================================================
    // Transaction RAII
    // ========================================================================
    struct Transaction {
        bool   started_transaction = false;
        Pager* p                   = nullptr;

        Transaction();
        explicit Transaction(Pager* in_pager);
        Transaction(Transaction&&);
        Transaction& operator=(Transaction&&);
        ~Transaction();

        Transaction(const Transaction&)            = delete;
        Transaction& operator=(const Transaction&) = delete;

        coroutine::Task<> begin();
        coroutine::Task<> commit();
        void              rollback(); // synchronous; used by destructor and explicit rollback callers
    };
}

export namespace plexdb {
    using Pager = pager::Pager;
}
