module;
#include <plexdb/macros/macros.h>
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

static_assert(PLEXDB_ARCH_LITTLE_ENDIAN, "storage format assumes little-endian byte order");

module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.plugin;
import plexdb.aio;
import plexdb.threads;
import plexdb.arena;

namespace plexdb::pager {
    // Forward declarations of internal helpers used before their definitions.
    static void rollback_transaction(Pager& pager);

    // ========================================================================
    // plugin instrumentation
    // ========================================================================
    static plugin::Producer pager_producer{"otlp.db.pager"};

    static plugin::Stat stat_tx_committed{&pager_producer, "transaction.committed", plugin::StatType::Counter};
    static plugin::Stat stat_tx_rolled_back{&pager_producer, "transaction.rolled_back", plugin::StatType::Counter};
    static plugin::Stat stat_checkpoint{&pager_producer, "checkpoint.executed", plugin::StatType::Counter};
    static plugin::Stat stat_cache_hit{&pager_producer, "cache.hit", plugin::StatType::Counter};
    static plugin::Stat stat_cache_miss{&pager_producer, "cache.miss", plugin::StatType::Counter};

    // ========================================================================
    // internal helpers
    // ========================================================================
    static coroutine::Task<> read_header(Pager& pager) {
        co_await aio::file_read(
            *pager.file_io_ctx,
            pager.file,
            Rng1U64{
                .start = pager.base_offset,
                .end   = pager.base_offset + sizeof(pager.header),
            },
            &pager.header
        );
    }

    coroutine::Task<Header> create(aio::FileIOContext& file_io_ctx, os::Handle file, U64 page_size, U64 base_offset) {
        assert_true(sizeof(Header) <= page_size, "header does not fit in root page");
        assert_true(page_size % sizeof(U64) == 0, "invalid page size alignment");

        Header header{
            .magic      = HEADER_MAGIC,
            .version    = HEADER_CURRENT_VERSION,
            .page_size  = page_size,
            .page_count = 1, // header page
            .root_page  = MAX_U64,
        };

        os::file_resize_zero(file, base_offset + header.page_count * header.page_size);
        co_await aio::file_write(file_io_ctx, file, Rng1U64{.start = base_offset, .end = base_offset + sizeof(header)}, &header);

        co_return header;
    }

    // @warn only use when wal is disabled
    static coroutine::Task<> commit_no_wal(Pager& pager) {
        aio::FileIOContext& ctx = *pager.file_io_ctx;

        struct WriteOp {
            U64 page_idx; // MAX_U64 = header entry
        };

        U64 dirty_count = 0;
        for (auto& pair : pager.page_cache) {
            if (pair.second.dirty && pair.first < pager.header.page_count) {
                dirty_count++;
            }
        }
        U64 write_count = dirty_count + (pager.header_in_write_set ? 1 : 0);

        if (write_count > 0) {
            threads::Scope      sc            = threads::scratch();
            WriteOp*            write_storage = arena::push_array_no_zero<WriteOp>(*sc.arena, write_count);
            TArrayView<WriteOp> writes{write_storage, write_count}; // @todo capped array

            U64 n = 0;
            for (auto& pair : pager.page_cache) {
                if (pair.second.dirty && pair.first < pager.header.page_count) {
                    writes[n++] = WriteOp{.page_idx = pair.first};
                }
            }
            if (pager.header_in_write_set) {
                writes[n++]               = WriteOp{.page_idx = MAX_U64};
                pager.header_in_write_set = false;
            }
            assert_true(n == write_count, "write count did not match op count, header or page became dirty inside flush");

            auto write_one = [&ctx, &pager](const WriteOp& w) -> coroutine::Task<> {
                if (w.page_idx == MAX_U64) {
                    co_await aio::file_write(ctx, pager.file, Rng1U64{.start = pager.base_offset, .end = pager.base_offset + sizeof(pager.header)}, &pager.header);
                    co_return;
                }
                auto* entry = find(pager.page_cache, w.page_idx);
                assert_true(entry != nullptr, "commit_no_wal: dirty page missing from page_cache during write");
                U64 offset = pager.base_offset + pager.header.page_size * w.page_idx;
                co_await aio::file_write(ctx, pager.file, Rng1U64{.start = offset, .end = offset + pager.header.page_size}, entry->data.ptr);
            };

            co_await coroutine::when_all(writes, write_one);
        }

        os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * pager.header.page_size);
        co_await aio::file_sync(ctx, pager.file);
        clear(pager.page_cache);
    }

    static coroutine::Task<> maybe_recover_wal(Pager& pager) {
        if (!pager.wal) {
            co_return;
        }
        if (!co_await wal::try_load(pager.wal, *pager.file_io_ctx, pager.header.page_size)) {
            co_return;
        }
        if (pager.wal.header.frame_count == 0) {
            co_return;
        }

        assert_true(pager.wal.header.magic == wal::MAGIC, "maybe_recover_wal: WAL header has invalid magic");

        U64 page_size = pager.wal.header.page_size;
        {
            U64            buf_size = max(page_size, static_cast<U64>(sizeof(Header)));
            threads::Scope sc       = threads::scratch();
            U8*            buf      = arena::push_array_no_zero<U8>(*sc.arena, buf_size);
            for (U64 i = 0; i < pager.wal.header.frame_count; i++) {
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, *pager.file_io_ctx, i, frame, buf);

                if (frame.page_idx == MAX_U64) {
                    co_await aio::file_write(*pager.file_io_ctx, pager.file, Rng1U64{.start = pager.base_offset, .end = pager.base_offset + sizeof(Header)}, buf);
                } else {
                    U64 db_start = pager.base_offset + page_size * frame.page_idx;
                    co_await aio::file_write(*pager.file_io_ctx, pager.file, Rng1U64{.start = db_start, .end = db_start + page_size}, buf);
                }
            }
        }

        co_await aio::file_sync(*pager.file_io_ctx, pager.file);

        pager.wal.header.frame_count = 0;
        co_await aio::file_write(
            *pager.file_io_ctx,
            pager.wal.file,
            Rng1U64{
                .start = offsetof(wal::Header, frame_count),
                .end   = offsetof(wal::Header, frame_count) + sizeof(U64)
            },
            &pager.wal.header.frame_count
        );
        co_await aio::file_sync(*pager.file_io_ctx, pager.wal.file);

        co_await read_header(pager);
        // truncate file to the page count recorded in the recovered header
        os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * pager.header.page_size);
    }

    Pager::Pager(aio::FileIOContext* file_io_ctx, os::Handle file, const Header& header, U64 checkpoint_interval, U64 base_offset)
        : file_io_ctx(file_io_ctx)
        , file(file)
        , base_offset(base_offset)
        , header(header)
        , checkpoint_interval(checkpoint_interval) {
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, U64 base_offset) {
        p.file_io_ctx = file_io_ctx;
        p.file        = file;
        p.base_offset = base_offset;
        co_await read_header(p);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, U64 base_offset) {
        p.file_io_ctx = file_io_ctx;
        p.file        = file;
        p.base_offset = base_offset;
        p.wal         = Wal{wal_file};
        co_await read_header(p);
        co_await maybe_recover_wal(p);
    }

    coroutine::Task<> init(Pager& p, aio::FileIOContext* file_io_ctx, os::Handle file, os::Handle wal_file, const Header& header, U64 base_offset) {
        wal::Header wal_hdr = co_await wal::create(*file_io_ctx, wal_file, header.page_size);
        p.file_io_ctx       = file_io_ctx;
        p.file              = file;
        p.base_offset       = base_offset;
        p.header            = header;
        p.wal               = Wal{wal_file, wal_hdr};
    }

    coroutine::Task<> destroy(Pager& p) {
        assert_true(p.file_io_ctx != nullptr, "destroy requires file_io_ctx");
        if (p.transaction_active) {
            rollback_transaction(p);
        } else if (!p.wal) {
            co_await commit_no_wal(p);
        }
        p.file_io_ctx = nullptr;
    }

    Pager::Pager(Pager&& other)
        : file_io_ctx(other.file_io_ctx)
        , file(other.file)
        , base_offset(other.base_offset)
        , header(other.header)
        , saved_header(other.saved_header)
        , header_in_write_set(other.header_in_write_set)
        , transaction_active(other.transaction_active)
        , tx_waiters(move(other.tx_waiters))
        , checkpoint_interval(other.checkpoint_interval)
        , wal_transaction_count(other.wal_transaction_count)
        , page_cache(move(other.page_cache))
        , wal(move(other.wal)) {
        other.file               = os::zero_handle();
        other.file_io_ctx        = nullptr;
        other.transaction_active = false;
    }

    Pager::~Pager() {
        assert_true(file_io_ctx == nullptr, "pager destroyed without destroy()");
    }

    Pager& Pager::operator=(Pager&& other) {
        if (this == &other) {
            return *this;
        }

        assert_true(file_io_ctx == nullptr, "pager overwritten without destroy()");

        file                  = other.file;
        base_offset           = other.base_offset;
        file_io_ctx           = other.file_io_ctx;
        header                = other.header;
        saved_header          = other.saved_header;
        header_in_write_set   = other.header_in_write_set;
        transaction_active    = other.transaction_active;
        tx_waiters            = move(other.tx_waiters);
        checkpoint_interval   = other.checkpoint_interval;
        wal_transaction_count = other.wal_transaction_count;
        page_cache            = move(other.page_cache);
        wal                   = move(other.wal);

        other.file               = os::zero_handle();
        other.file_io_ctx        = nullptr;
        other.transaction_active = false;

        return *this;
    }

    // ========================================================================
    // internal transaction helpers
    // ========================================================================

    static void finish_transaction(Pager& pager) {
        pager.transaction_active = false;
        if (pager.tx_waiters.length > 0) {
            auto h = *front(pager.tx_waiters);
            pop_front(pager.tx_waiters);
            h.resume();
        }
    }

    static coroutine::Task<> begin_transaction(Pager& pager) {
        if (pager.transaction_active) {
            // Block scope ensures node outlives ~Awaitable (reverse construction order).
            Deque<std::coroutine_handle<>>::Node node{};
            co_await coroutine::Awaitable{
                [p = &pager, n = &node](std::coroutine_handle<> h) { n->value = h; push_back(p->tx_waiters, n); },
                []() {},
                [p = &pager, n = &node]() {
                    remove(p->tx_waiters, n);
                }
            };
        }
        pager.saved_header       = pager.header;
        pager.transaction_active = true;
    }

    static coroutine::Task<> commit_transaction(Pager& pager) {
        assert_true(pager.transaction_active, "no active transaction");

        if (!pager.wal) {
            co_await commit_no_wal(pager);
            finish_transaction(pager);
            plugin::stat(stat_tx_committed, 1);
            plugin::message(pager_producer, plugin::Level::Debug, fmt("pager: transaction committed, page_count=%" PRIu64, pager.header.page_count));
            co_return;
        }

        aio::FileIOContext& ctx = *pager.file_io_ctx;

        // @note gathering ops does not suspend so page state should not change
        U64 dirty_count = 0;
        for (auto& pair : pager.page_cache) {
            if (pair.second.dirty && pair.first < pager.header.page_count) {
                dirty_count++;
            }
        }
        U64 write_count = dirty_count + (pager.header_in_write_set ? 1 : 0);

        if (write_count > 0) {
            threads::Scope             sc            = threads::scratch();
            wal::PageWrite*            write_storage = arena::push_array_no_zero<wal::PageWrite>(*sc.arena, write_count);
            TArrayView<wal::PageWrite> writes{write_storage, write_count};

            U64 n = 0;
            for (auto& pair : pager.page_cache) {
                if (pair.second.dirty && pair.first < pager.header.page_count) {
                    writes[n++] = wal::PageWrite{.page_idx = pair.first, .data = pair.second.data.ptr};
                }
            }
            if (pager.header_in_write_set) {
                writes[n++]               = wal::PageWrite{.page_idx = MAX_U64, .data = reinterpret_cast<const U8*>(&pager.header)};
                pager.header_in_write_set = false;
            }
            assert_true(n == write_count, "commit_transaction: dirty-page count changed between counting and gathering pass");

            U64 base_frame_idx = pager.wal.header.frame_count;
            co_await wal::append_frames(pager.wal, ctx, writes);

            for (U64 write_idx = 0; write_idx < write_count; write_idx++) {
                insert(pager.wal.wal_index, writes[write_idx].page_idx, base_frame_idx + write_idx);
            }
        }

        if (pager.wal.header.frame_count > 0) {
            co_await wal::commit(pager.wal, ctx);
            pager.wal_transaction_count++;
            if (pager.wal_transaction_count >= pager.checkpoint_interval) {
                co_await checkpoint(pager);
            }
        }

        clear(pager.page_cache);
        finish_transaction(pager);
        plugin::stat(stat_tx_committed, 1);
        plugin::message(pager_producer, plugin::Level::Debug, fmt("pager: transaction committed, page_count=%" PRIu64, pager.header.page_count));
    }

    static void rollback_transaction(Pager& pager) {
        plugin::message(pager_producer, plugin::Level::Error, "pager: transaction rolled back");
        plugin::stat(stat_tx_rolled_back, 1);
        pager.header              = pager.saved_header;
        pager.header_in_write_set = false;
        clear(pager.page_cache);
        // WAL contains only committed-transaction data; dirty pages from this
        // rolled-back transaction live only in the page cache (already cleared above).
        // Do NOT reset the WAL — that would destroy previously committed frames.
        finish_transaction(pager);
    }

    // ========================================================================
    // RAII Transaction
    // ========================================================================
    Transaction::Transaction()
        : p(nullptr) {
    }

    Transaction::Transaction(Pager* in_pager)
        : p(in_pager) {
        assert_true(in_pager != nullptr, "cannot create a Transaction from a nullptr");
    }

    Transaction::Transaction(Transaction&& other)
        : started_transaction(other.started_transaction)
        , p(other.p) {
        other.started_transaction = false;
        other.p                   = nullptr;
    }

    Transaction& Transaction::operator=(Transaction&& other) {
        this->started_transaction = other.started_transaction;
        this->p                   = other.p;
        other.started_transaction = false;
        other.p                   = nullptr;
        return *this;
    }

    Transaction::~Transaction() {
        if (started_transaction && p && p->transaction_active) {
            plugin::message(pager_producer, plugin::Level::Error, "pager: Transaction destroyed with active transaction — rolling back");
            rollback();
        }
    }

    coroutine::Task<> Transaction::begin() {
        assert_true(p != nullptr, "Transaction::begin called with null pager");
        assert_true(!started_transaction, "Transaction::begin called on already-started transaction (double-begin)");
        co_await begin_transaction(*p);
        started_transaction = true;
        plugin::message(pager_producer, plugin::Level::Debug, fmt("pager: transaction begun, page_count=%" PRIu64, p->header.page_count));
    }

    coroutine::Task<> Transaction::commit() {
        assert_true(started_transaction, "Transaction::commit called before begin");
        co_await commit_transaction(*p);
        started_transaction = false;
    }

    void Transaction::rollback() {
        rollback_transaction(*p);
        started_transaction = false;
    }

    // ========================================================================
    // checkpoint
    // ========================================================================
    coroutine::Task<> checkpoint(Pager& pager) {
        ZoneScopedN("pager::checkpoint");
        assert_true(pager.wal, "checkpoint requires WAL");
        assert_true(pager.file_io_ctx != nullptr, "checkpoint requires file_io_ctx");

        aio::FileIOContext& ctx       = *pager.file_io_ctx;
        U64                 page_size = pager.wal.header.page_size;

        U64 entry_count = length(pager.wal.wal_index);

        if (entry_count > 0) {
            struct CheckpointWrite {
                U64 page_idx; // MAX_U64 = header entry; index into wal_index/page_cache
                U64 buf_idx;  // MAX_U64 = no wal read needed (page is cached, or this is the header entry)
            };

            U64 buf_size = max(page_size, static_cast<U64>(sizeof(Header)));

            threads::Scope sc = threads::scratch();

            CheckpointWrite*            write_storage = arena::push_array_no_zero<CheckpointWrite>(*sc.arena, entry_count);
            TArrayView<CheckpointWrite> writes{write_storage, entry_count}; // @todo capped array

            U64 n          = 0;
            U64 buf_needed = 0;
            for (auto& pair : pager.wal.wal_index) {
                U64  page_idx  = pair.first;
                bool need_read = page_idx != MAX_U64 && find(pager.page_cache, page_idx) == nullptr;
                writes[n++]    = CheckpointWrite{
                       .page_idx = page_idx,
                       .buf_idx  = need_read ? buf_needed++ : MAX_U64,
                };
            }
            assert_true(n == entry_count, "wal index entry count changed between inside gathering pass");

            U8* read_bufs = arena::push_array_no_zero<U8>(*sc.arena, buf_needed * buf_size);

            auto write_one = [&ctx, &pager, page_size, read_bufs, buf_size](const CheckpointWrite& w) -> coroutine::Task<> {
                if (w.page_idx == MAX_U64) {
                    co_await aio::file_write(ctx, pager.file, Rng1U64{.start = pager.base_offset, .end = pager.base_offset + sizeof(Header)}, &pager.header);
                    co_return;
                }
                U64 db_start = pager.base_offset + page_size * w.page_idx;
                if (w.buf_idx == MAX_U64) {
                    auto* entry = find(pager.page_cache, w.page_idx);
                    assert_true(entry != nullptr, "checkpoint: page missing from page_cache during write");
                    co_await aio::file_write(ctx, pager.file, Rng1U64{.start = db_start, .end = db_start + page_size}, entry->data.ptr);
                    co_return;
                }
                U64* frame_idx = find(pager.wal.wal_index, w.page_idx);
                assert_true(frame_idx != nullptr, "checkpoint: page missing from wal_index during write");
                U8*        buf = read_bufs + w.buf_idx * buf_size;
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, ctx, *frame_idx, frame, buf);
                co_await aio::file_write(ctx, pager.file, Rng1U64{.start = db_start, .end = db_start + page_size}, buf);
            };

            co_await coroutine::when_all(writes, write_one);
        }

        os::file_resize_zero(pager.file, pager.base_offset + pager.header.page_count * page_size);
        co_await aio::file_sync(ctx, pager.file);
        co_await wal::reset(pager.wal, ctx);
        clear(pager.wal.wal_index);
        pager.wal_transaction_count = 0;
        plugin::stat(stat_checkpoint, 1);
    }

    // ========================================================================
    // page operations
    // ========================================================================
    coroutine::Task<> set_root(Pager& pager, U64 page) {
        assert_true(pager.transaction_active, "no transaction active");
        pager.header.root_page    = page;
        pager.header_in_write_set = true;
        co_return;
    }

    coroutine::Task<const U8*> rpage(Pager& pager, U64 idx) {
        assert_true(pager.transaction_active, "rpage requires an active transaction");
        assert_true(idx > 0, "page index must be > 0");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rpage requires file_io_ctx");

        if (auto* entry = find(pager.page_cache, idx)) {
            plugin::stat(stat_cache_hit, 1);
            co_return entry->data.ptr;
        }

        plugin::stat(stat_cache_miss, 1);
        U8*  data   = os::allocate_zero(pager.header.page_size);
        bool loaded = false;
        if (pager.wal) {
            if (U64* frame_idx_ptr = find(pager.wal.wal_index, idx)) {
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, *pager.file_io_ctx, *frame_idx_ptr, frame, data);
                loaded = true;
            }
        }
        if (!loaded) {
            auto offset = pager.base_offset + pager.header.page_size * idx;
            co_await aio::file_read(
                *pager.file_io_ctx, pager.file,
                Rng1U64{.start = offset, .end = offset + pager.header.page_size},
                data
            );
        }

        // @note a concurrent rpage/rwpage for the same idx may have raced ahead and
        // inserted its own entry while we awaited disk I/O; reuse it instead of
        // overwriting, which would free a buffer the racing coroutine still holds.
        if (auto* entry = find(pager.page_cache, idx)) {
            os::deallocate(data);
            co_return entry->data.ptr;
        }
        insert(pager.page_cache, idx, PageCacheEntry{UniquePtr<U8>{data}, false});
        co_return data;
    }

    coroutine::Task<U8*> rwpage(Pager& pager, U64 idx) {
        assert_true(pager.transaction_active, "no transaction active");
        assert_true(idx > 0, "page index must be > 0");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        assert_true(pager.file_io_ctx != nullptr, "rwpage requires file_io_ctx");

        if (auto* entry = find(pager.page_cache, idx)) {
            plugin::stat(stat_cache_hit, 1);
            entry->dirty = true;
            co_return entry->data.ptr;
        }

        plugin::stat(stat_cache_miss, 1);
        U8*  data   = os::allocate_zero(pager.header.page_size);
        bool loaded = false;
        if (pager.wal) {
            if (U64* frame_idx_ptr = find(pager.wal.wal_index, idx)) {
                wal::Frame frame{};
                co_await wal::read_frame(pager.wal, *pager.file_io_ctx, *frame_idx_ptr, frame, data);
                loaded = true;
            }
        }
        if (!loaded) {
            auto offset = pager.base_offset + pager.header.page_size * idx;
            co_await aio::file_read(
                *pager.file_io_ctx, pager.file,
                Rng1U64{.start = offset, .end = offset + pager.header.page_size},
                data
            );
        }

        // @note see matching comment in rpage: reuse a racing coroutine's entry
        // instead of overwriting it out from under that coroutine.
        if (auto* entry = find(pager.page_cache, idx)) {
            os::deallocate(data);
            entry->dirty = true;
            co_return entry->data.ptr;
        }
        insert(pager.page_cache, idx, PageCacheEntry{UniquePtr<U8>{data}, true});
        co_return data;
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
        return sizeof(U64) * 8;
    }
    inline static coroutine::Task<const U64*> rbitset(Pager& pager, U64 idx) {
        co_return reinterpret_cast<const U64*>(co_await rpage(pager, idx));
    }
    inline static coroutine::Task<U64*> rwbitset(Pager& pager, U64 idx) {
        co_return reinterpret_cast<U64*>(co_await rwpage(pager, idx));
    }

    // Guarantee that page `idx` has a zeroed, dirty cache entry so rwpage returns clean state.
    // Called from new_page to defend against stale WAL/file content on reused pages.
    static void ensure_page_zeroed(Pager& pager, U64 idx) {
        if (auto* entry = find(pager.page_cache, idx)) {
            os::memory_zero(entry->data.ptr, pager.header.page_size);
            entry->dirty = true;
        } else {
            U8* data = static_cast<U8*>(os::allocate_zero(pager.header.page_size));
            insert(pager.page_cache, idx, PageCacheEntry{UniquePtr<U8>{data}, true});
        }
    }

    coroutine::Task<U64> new_page(Pager& pager) {
        ZoneScopedN("pager::new_page");
        assert_true(pager.transaction_active, "new_page requires an active transaction");
        const U64 ENTRIES_PER_BITSET = get_bitset_entry_count(pager);
        const U64 BITSET_PAGE_STRIDE = ENTRIES_PER_BITSET * get_pages_per_entry() + 1;
        auto&     h                  = pager.header;

        for (U64 bitset_page = 1; bitset_page < h.page_count; bitset_page += BITSET_PAGE_STRIDE) {
            U64* bitset = co_await rwbitset(pager, bitset_page);

            for (U64 entry_idx = 0; entry_idx < ENTRIES_PER_BITSET; entry_idx++) {
                if (~bitset[entry_idx] != 0) {
                    U64 bit_idx = bit_count_trailing_zeros(~bitset[entry_idx]);
                    bitset[entry_idx] |= 1_u64 << bit_idx;

                    U64 free_page = bitset_page + 1 + get_pages_per_entry() * entry_idx + bit_idx;
                    if (free_page >= h.page_count) {
                        h.page_count              = free_page + 1;
                        pager.header_in_write_set = true;
                        os::file_resize_zero(pager.file, pager.base_offset + h.page_count * h.page_size);
                    }
                    ensure_page_zeroed(pager, free_page);
                    co_return free_page;
                }
            }
        }

        assert_true((h.page_count - 1) % BITSET_PAGE_STRIDE == 0, "bitset correctly aligned");

        U64 bitset_page           = h.page_count++;
        U64 free_page             = h.page_count++;
        pager.header_in_write_set = true;
        os::file_resize_zero(pager.file, pager.base_offset + h.page_count * h.page_size);

        U64* bitset = co_await rwbitset(pager, bitset_page);
        bitset[0] |= 1_u64;

        ensure_page_zeroed(pager, free_page);
        co_return free_page;
    }

    coroutine::Task<> delete_page(Pager& pager, U64 idx) {
        ZoneScopedN("pager::delete_page");
        assert_true(pager.transaction_active, "delete_page requires an active transaction");
        assert_true(idx < pager.header.page_count && idx > 0, "page out of range");
        const U64 PAGES_PER_BITSET = get_bitset_entry_count(pager) * get_pages_per_entry() + 1;
        auto&     h                = pager.header;

        U64 bitset_page = ((idx - 1) / PAGES_PER_BITSET) * PAGES_PER_BITSET + 1;
        assert_true(idx != bitset_page, "invalid delete page");
        U64* bitset = co_await rwbitset(pager, bitset_page);

        U64 bitset_idx = idx - bitset_page - 1;
        U64 entry_idx  = bitset_idx / get_pages_per_entry();
        U64 bit_idx    = bitset_idx % get_pages_per_entry();
        assert_true(bitset[entry_idx] & 1_u64 << bit_idx, "trying to delete page marked as deleted");
        bitset[entry_idx] &= ~(1_u64 << bit_idx);

        // Zero the page so future allocations of the same page index (in the same or a later
        // transaction) always start with clean state instead of stale node data (e.g. non-zero
        // key_count). Mark dirty so the zeroed content is committed to the WAL, ensuring cross-
        // transaction reuse also sees zeroed data (not old file contents).
        if (auto* entry = find(pager.page_cache, idx)) {
            os::memory_zero(entry->data.ptr, pager.header.page_size);
            entry->dirty = true;
        } else {
            U8* data = static_cast<U8*>(os::allocate_zero(pager.header.page_size));
            insert(pager.page_cache, idx, PageCacheEntry{UniquePtr<U8>{data}, true});
        }

        if (idx != h.page_count - 1) {
            co_return;
        }

        const U64* bitset_r = bitset;
        PLEXDB_DEBUG_X(U64 old_page_count = h.page_count);
        while (true) {
            for (;; entry_idx--) {
                if (bitset_r[entry_idx] != 0) {
                    U64 occupied_count = get_pages_per_entry() - bit_count_leading_zeros(bitset_r[entry_idx]);
                    h.page_count       = bitset_page + 1 + entry_idx * get_pages_per_entry() + occupied_count;
                    break;
                }
                if (entry_idx == 0) {
                    h.page_count = bitset_page;
                    break;
                }
            }

            if (h.page_count != bitset_page || h.page_count <= PAGES_PER_BITSET) {
                break;
            }
            bitset_page -= PAGES_PER_BITSET;
            bitset_r  = co_await rbitset(pager, bitset_page);
            entry_idx = get_bitset_entry_count(pager) - 1;
        }
        PLEXDB_DEBUG_X(assert_true(h.page_count < old_page_count, "trimming actually trimmed"));
        // @note file is actually truncated at checkpoint / commit
        pager.header_in_write_set = true;
    }
}
