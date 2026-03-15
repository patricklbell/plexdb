export module plexdb.os.uring;

import plexdb.os;
import plexdb.base;
import plexdb.tagged_union;

using namespace plexdb;

export namespace plexdb::uring {
    // ========================================================================
    // ring
    // ========================================================================
    struct RingSettings {
        bool recommended;
        U32 recommended_queue_depth;
        U64 recommended_buffer_size;
        U32 recommended_buffer_count;
        
        bool available;
        U32 available_queue_depth;
        U64 available_buffer_bytes;
        U32 available_buffer_count;
        U32 available_ring_count;
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
        os::Handle server = os::zero_handle();
        void* ring_vptr = nullptr;
        U8* buffers = nullptr;
        void* iovecs_vptr = nullptr;
        U64 buffer_size = 0;
        U32 buffer_count = 0;

        Ring() = default;
        Ring(os::Handle server, U32 queue_depth, U64 buffer_size, U32 buffer_count);
        ~Ring();

        Ring(const Ring&) = delete;
        Ring& operator=(const Ring&) = delete;

        Ring(Ring&& other) noexcept;
        Ring& operator=(Ring&& other) noexcept;

        explicit operator bool() const;
    };

    U8* get_buffer_ptr(Ring& ring, U32 buffer_idx);

    // ========================================================================
    // completion queue entry
    // ========================================================================

    struct ReadEvent {
        U32 buffer_idx;
        int bytes_read;
    };
    struct WriteEvent {
        U32 buffer_idx;
        int bytes_written;
    };
    struct AcceptEvent {
        os::Handle client;
    };
    struct MultishotAcceptEvent {
        os::Handle client;
    };
    struct CloseEvent {
        os::Handle client;
    };

    // Completion for any io_uring operation submitted with a coro::IoAwaitable
    // pointer encoded in user_data.  The event loop calls
    // IoAwaitable::complete(result) to resume the waiting coroutine.
    struct CoroEvent {
        void* awaitable_ptr;
        int   result;
    };

    using CQE = TaggedUnion<ReadEvent,WriteEvent,AcceptEvent,MultishotAcceptEvent,CloseEvent,CoroEvent>;

    U32 cqe_get_size(const Ring& ring);
    CQE cqe_top(Ring& ring);
    bool cqe_pop(Ring& ring, U32 count = 0);

    // ========================================================================
    // submission queue entry
    // ========================================================================
    bool sqe_push_accept(Ring& ring);
    bool sqe_push_multishot_accept(Ring& ring);
    bool sqe_push_close(Ring& ring, os::Handle client);

    bool sqe_push_read(Ring& ring, os::Handle client, U32 buffer_idx);
    bool sqe_push_write(Ring& ring, os::Handle client, U32 buffer_idx, U32 offset, U32 byte_count);

    // ========================================================================
    // coroutine-compatible SQE operations
    //   These encode an IoAwaitable pointer in user_data (TYPE_CORO tag).
    //   The event loop decodes it and calls IoAwaitable::complete(result)
    //   to resume the waiting coroutine.
    // ========================================================================

    // Recv from a registered buffer; awaitable is resumed when data arrives.
    bool sqe_push_coro_recv(Ring& ring, os::Handle client, U32 buffer_idx, void* awaitable_ptr);

    // Send from a registered buffer; awaitable is resumed when the send completes.
    bool sqe_push_coro_send(Ring& ring, os::Handle client, U32 buffer_idx, U32 offset, U32 byte_count, void* awaitable_ptr);

    // Async file read/write (unregistered buffer) for coroutine-based pager IO.
    // @note Uses io_uring_prep_read/write (not fixed), so no buffer registration required.
    bool sqe_push_file_read(Ring& ring, os::Handle file, U64 file_offset, void* buf, U32 byte_count, void* awaitable_ptr);
    bool sqe_push_file_write(Ring& ring, os::Handle file, U64 file_offset, const void* buf, U32 byte_count, void* awaitable_ptr);

    bool sqe_submit_non_blocking(Ring& ring);

    // ========================================================================
    // stats
    // ========================================================================
    struct Stats {
        U64 queue_depth;
        U64 entries;
    };
    
    Stats get_stats(const Ring& ring);
}