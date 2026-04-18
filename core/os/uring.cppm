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
    enum class Error {
        None = 0,       // success
        NotFound,       // resource not found
        Invalid,        // invalid argument
        IO,             // generic I/O error
        Unknown         // unmapped or unknown error
    };

    struct ReadEvent {
        Error error;
        U32 buffer_idx;
        U32 bytes_read;
    };
    struct WriteEvent {
        Error error;
        U32 buffer_idx;
        U32 bytes_written;
    };
    struct AcceptEvent {
        Error error;
        os::Handle client;
    };
    struct MultishotAcceptEvent {
        Error error;
        os::Handle client;
    };
    struct CloseEvent {
        Error error;
        os::Handle client;
    };

    using CQE = TaggedUnion<ReadEvent,WriteEvent,AcceptEvent,MultishotAcceptEvent,CloseEvent>;

    U32 cqe_get_size(const Ring& ring);
    CQE cqe_top(Ring& ring);
    bool cqe_pop(Ring& ring, U32 count = 0);

    // ========================================================================
    // submission queue entry
    // ========================================================================
    bool sqe_push_accept(Ring& ring);
    bool sqe_push_multishot_accept(Ring& ring);
    bool sqe_push_close(Ring& ring, os::Handle client);

    bool sqe_push_read(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count);
    bool sqe_push_write(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count);

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