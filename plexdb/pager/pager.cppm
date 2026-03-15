module;
#include "macros.h"
#include <coroutine>

export module plexdb.pager;

import plexdb.base;
import plexdb.os;
import plexdb.os.uring;
import plexdb.arena;
import plexdb.coro;

export namespace plexdb::pager {
    constexpr U64 DEFAULT_READ_CACHE = 1000u;
    constexpr U8 HEADER_MAGIC[6] = {'p', 'X', 'D', 'b', 1, 12};
    constexpr U8 HEADER_CURRENT_VERSION[2] = { 0, 1 };

    // @todo lru
    // @todo experiment with segmented LFU or CLOCK
    struct Pager {
        static constexpr U64 alignment = sizeof(U64);
        os::Handle file = os::zero_handle();
        U64 base_offset = 0;

        // @padding
        struct Header {
            U8 magic[sizeof(HEADER_MAGIC)];
            U8 version[sizeof(HEADER_CURRENT_VERSION)];
            U64 page_size;
            U64 page_count;
            U64 root_page;
        } header = {0};
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

        Pager() = default;
        Pager(os::Handle file, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
        Pager(os::Handle file, const Pager::Header& header, U64 base_offset=0, U64 read_cache=DEFAULT_READ_CACHE);
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

    // ========================================================================
    // Async IO (coroutine-based, requires io_uring Ring)
    //   These functions yield from the calling coroutine while the page IO is
    //   in flight, allowing the event loop to service other connections.
    //
    //   Pattern:
    //       const U8* data = co_await pager::rpage_async(pager, ring, idx);
    //
    //   @note The Ring must have been initialised with a queue depth large
    //         enough to hold the page IO alongside network SQEs.
    //   @note The Ring's registered-buffer pool is NOT used; pager::rpage_async
    //         reads directly into the pager's own read-cache buffers (no extra
    //         copy).  This avoids registering the read cache with io_uring.
    // ========================================================================
    coro::Task rpage_async(Pager& pager, uring::Ring& ring, U64 idx,
                           const U8** out_ptr, coro::EventLoop& loop);
    coro::Task wpage_async(Pager& pager, uring::Ring& ring, U64 idx,
                           coro::EventLoop& loop);
}

export namespace plexdb {
    using Pager = pager::Pager;
}