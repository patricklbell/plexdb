module;
#include <coroutine>

export module plexdb.os.uring;

import plexdb.os;
import plexdb.base;
import plexdb.tagged_union;
import plexdb.coroutine;

using namespace plexdb;

export namespace plexdb::uring {
    // ========================================================================
    // ring
    // ========================================================================
    struct RingSettings {
        bool recommended;
        U32  recommended_queue_depth;
        U64  recommended_buffer_size;
        U32  recommended_buffer_count;

        bool available;
        U32  available_queue_depth;
        U64  available_buffer_bytes;
        U32  available_buffer_count;
        U32  available_ring_count;
    };

    // @todo thread safe
    const RingSettings* get_ring_settings();

    // @note Ring requires io_uring with the following features:
    // - IORING_FEAT_FAST_POLL: efficient polling for network operations
    // - IORING_OP_ACCEPT: accept operation support
    // - IORING_OP_RECV: receive operation support
    // - IORING_OP_SEND: send operation support
    // - IORING_OP_CLOSE: close operation support
    // - Multishot accept support (kernel 5.19+)
    struct Ring {
        os::Handle server       = os::zero_handle();
        os::Handle event_fd     = os::zero_handle(); // readable when CQEs are available (for epoll integration)
        void*      ring_vptr    = nullptr;
        U8*        buffers      = nullptr;
        void*      iovecs_vptr  = nullptr;
        U64        buffer_size  = 0;
        U32        buffer_count = 0;

        Ring() = default;
        Ring(os::Handle server, U32 queue_depth, U64 buffer_size, U32 buffer_count);
        ~Ring();

        Ring(const Ring&)            = delete;
        Ring& operator=(const Ring&) = delete;

        Ring(Ring&& other) noexcept;
        Ring& operator=(Ring&& other) noexcept;

        explicit operator bool() const;
    };

    U8* get_buffer_ptr(Ring& ring, U32 buffer_idx);

    // ========================================================================
    // completion queue entry
    // ========================================================================
    enum class Error {
        None = 0, // success
        NotFound, // resource not found
        Invalid,  // invalid argument
        IO,       // generic I/O error
        Unknown   // unmapped or unknown error
    };

    struct ReadEvent {
        Error error;
        U32   buffer_idx;
        U32   bytes_read;
    };
    struct WriteEvent {
        Error error;
        U32   buffer_idx;
        U32   bytes_written;
    };
    struct AcceptEvent {
        Error      error;
        os::Handle client;
    };
    struct MultishotAcceptEvent {
        Error      error;
        os::Handle client;
    };
    struct CloseEvent {
        Error      error;
        os::Handle client;
    };

    struct FileReadEvent {
        Error error;
        U32   buffer_idx;
        U32   bytes_read;
        U64   token;
    };
    struct FileWriteEvent {
        Error error;
        U32   buffer_idx;
        U32   bytes_written;
        U64   token;
    };
    struct FileSyncEvent {
        Error error;
        U64   token;
    };

    using CQE = TaggedUnion<ReadEvent, WriteEvent, AcceptEvent, MultishotAcceptEvent, CloseEvent,
                            FileReadEvent, FileWriteEvent, FileSyncEvent>;

    U32  cqe_get_size(const Ring& ring);
    CQE  cqe_top(Ring& ring);
    bool cqe_pop(Ring& ring, U32 count = 0);

    // ========================================================================
    // submission queue entry
    // ========================================================================
    bool sqe_push_accept(Ring& ring);
    bool sqe_push_multishot_accept(Ring& ring);
    bool sqe_push_close(Ring& ring, os::Handle client);

    bool sqe_push_read(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count);
    bool sqe_push_write(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count);

    bool sqe_push_file_read(Ring& ring, os::Handle file, U32 buffer_idx, U64 file_offset, U32 count, U64 token);
    bool sqe_push_file_write(Ring& ring, os::Handle file, U32 buffer_idx, U64 file_offset, U32 count, U64 token);
    bool sqe_push_file_sync(Ring& ring, os::Handle file, U64 token);

    bool sqe_submit_non_blocking(Ring& ring);

    // ========================================================================
    // global I/O budget
    // ========================================================================
    struct GlobalIOBudget {
        U32 network_queue_depth;
        U64 network_buffer_size;
        U32 network_buffer_count;
        U32 file_queue_depth;
        U64 file_buffer_size;
        U32 file_buffer_count;
    };

    GlobalIOBudget compute_io_budget(U64 page_size);

    // ========================================================================
    // stats
    // ========================================================================
    struct Stats {
        U64 queue_depth;
        U64 entries;
    };

    Stats get_stats(const Ring& ring);

    // Reads from the ring's event_fd to clear its "readable" state.
    // Returns true if events were pending. Must be called after epoll fires on event_fd.
    bool ring_drain_event_fd(Ring& ring);

    // ========================================================================
    // buffer pool
    // ========================================================================
    constexpr U32 INVALID_BUFFER_IDX = MAX_U32;

    // Manages a fixed pool of ring buffers with coroutine back-pressure.
    // MaxWaiters bounds the number of coroutines that can simultaneously wait
    // for a free buffer slot.
    template<U64 MaxWaiters>
    struct BufferPool {
        U32                                           buffer_count = 0;
        U32                                           next_free    = 0;
        DynamicArray<bool>                            in_use;
        RingFifo<std::coroutine_handle<>, MaxWaiters> waiters;

        explicit BufferPool(U32 count)
            : buffer_count(count)
            , next_free(0)
            , in_use(count) {
        }

        U32 try_acquire() {
            U32 start = next_free;
            U32 cur   = start;
            do {
                if (!in_use[cur]) {
                    in_use[cur] = true;
                    next_free   = (cur + 1) % buffer_count;
                    return cur;
                }
                cur = (cur + 1) % buffer_count;
            } while (cur != start);
            return INVALID_BUFFER_IDX;
        }

        coroutine::Task<U32> acquire() {
            while (true) {
                U32 idx = try_acquire();
                if (idx != INVALID_BUFFER_IDX) {
                    co_return idx;
                }
                co_await coroutine::Awaitable{
                    [this](std::coroutine_handle<> h) { push_front(waiters, h); },
                    []() {
                    }};
            }
        }

        void release(U32 idx) {
            in_use[idx] = false;
            if (!empty(waiters)) {
                pop_front(waiters).resume();
            }
        }
    };
}