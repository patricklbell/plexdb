module;
#include "macros.h"

#if CORE_ENABLE_IO_URING
    #if !PLEXDB_OS_LINUX
        #error "Invalid build configuration, io_uring support is enabled but detected os was not linux"
    #endif
    #include <liburing.h>
    #include <sys/socket.h>
    #include <sys/mman.h>
    #include <stdlib.h>
    #include <unistd.h>
#endif

module plexdb.os.uring;

import plexdb.base;
import plexdb.os.socket;

namespace plexdb::uring {
    // ========================================================================
    // helpers
    // ========================================================================
    // Event type encoding in upper bits of user_data
    enum EventTypeTag : U64 {
        TYPE_ACCEPT             = 0x1_u64 << 60,
        TYPE_READ               = 0x2_u64 << 60,
        TYPE_WRITE              = 0x3_u64 << 60,
        TYPE_CLOSE              = 0x4_u64 << 60,
        TYPE_MULTISHOT_ACCEPT   = 0x5_u64 << 60,

        TYPE_MASK  = 0xF_u64 << 60,
        DATA_MASK = ~TYPE_MASK
    };

    static U64 encode_user_data(EventTypeTag type, U64 data) {
        return type | (data & DATA_MASK);
    }

    static EventTypeTag decode_type(U64 user_data) {
        return (EventTypeTag)(user_data & TYPE_MASK);
    }

    static U64 decode_data(U64 user_data) {
        return user_data & DATA_MASK;
    }

    Optional<RingSettings> g_ring_settings_opt;

    // call after making changes to ring settings
    static void finalize_ring_settings(RingSettings* settings) {
        constexpr U64 minimum_recommended_buffer_size = 1_kb;

        U64 page_size = os::get_system_info()->page_size;

        settings->available = os::get_kernel_features()->io_uring.supported &&
            (settings->available_buffer_bytes > 0) &&
            (settings->available_buffer_count > 0) &&
            (settings->available_ring_count > 0) && 
            (page_size >= minimum_recommended_buffer_size);

        // @note buffer_size must be page-aligned for io_uring_register_buffers mlock accounting
        settings->recommended = settings->available;
        if (!settings->recommended) {
            settings->recommended_queue_depth = 0;
            settings->recommended_buffer_size = 0;
            settings->recommended_buffer_count = 0;
            return;
        }

        settings->recommended_queue_depth = settings->available_queue_depth;
        settings->recommended_buffer_size = align_down(settings->available_buffer_bytes / settings->available_buffer_count, page_size);
        settings->recommended_buffer_count = min(
            settings->available_buffer_bytes / max(settings->recommended_buffer_size, 1_u64),
            settings->available_buffer_count
        );

        if (settings->recommended_buffer_size < minimum_recommended_buffer_size) {
            settings->recommended_buffer_size = align_down(minimum_recommended_buffer_size, page_size);
            settings->recommended_buffer_count = min(
                settings->available_buffer_bytes / max(minimum_recommended_buffer_size, 1_u64),
                settings->available_buffer_count
            );

            if (settings->recommended_buffer_count == 0 || settings->recommended_buffer_size == 0) {
                settings->recommended = false;
            }
        }
    }

    static RingSettings* get_ring_settings_internal() {
        if (!g_ring_settings_opt) {
            // @note reserve 64KB for io_uring ring structures (SQ/CQ rings, kernel overhead)
            constexpr U64 IO_URING_OVERHEAD_BYTES = 64_kb;
            U64 mlock_limit = os::get_system_info()->mlock_limit;
            U64 usable_mlock = max(mlock_limit, IO_URING_OVERHEAD_BYTES) - IO_URING_OVERHEAD_BYTES;

            RingSettings ring_settings = {
                .recommended = false,
                .recommended_queue_depth = 0,
                .recommended_buffer_size = 0,
                .recommended_buffer_count = 0,
                .available = false,
                .available_queue_depth = min(
                    os::get_kernel_features()->io_uring.max_sq_entries,
                    os::get_kernel_features()->io_uring.max_cq_entries
                ),
                .available_buffer_bytes = min(
                    usable_mlock,
                    os::get_system_info()->total_memory
                ),
                // @note assumes we register an iovec for each buffer
                .available_buffer_count = os::get_kernel_features()->io_uring.max_iovecs,
                .available_ring_count = os::get_system_info()->vma_limit,
            };

            finalize_ring_settings(&ring_settings);
            g_ring_settings_opt.emplace(move(ring_settings));
        }

        return &(*g_ring_settings_opt);
    }

    const RingSettings* get_ring_settings() {
        return get_ring_settings_internal();
    }

    U8* get_buffer_ptr(Ring& ring, U32 buffer_idx) {
        return &ring.buffers[buffer_idx*ring.buffer_size];
    }

    #if CORE_ENABLE_IO_URING
        static int handle_to_fd(os::Handle h) { return static_cast<int>(h.u32[0]); }
        static os::Handle fd_to_handle(int fd) { assert_true(fd >= 0, "invalid fd"); return os::Handle{.u32={static_cast<U32>(fd)}}; }

        // ========================================================================
        // ring
        // ========================================================================

        Ring::Ring(os::Handle server, U32 queue_depth, U64 buffer_size, U32 buffer_count)
            : server(server)
            , buffer_size(buffer_size)
            , buffer_count(buffer_count) {
            assert_true(buffer_size > 0, "zero buffer size");
            assert_true(buffer_count > 0, "zero buffer count");
            
            // allocate memory - must be page-aligned for io_uring_register_buffers
            U64 page_size = os::get_system_info()->page_size;
            int err = posix_memalign((void**)&this->buffers, page_size, buffer_size*buffer_count);
            if (err != 0) {
                return;
            }
            this->ring_vptr = os::allocate_zero(sizeof(io_uring));
            if (this->ring_vptr == nullptr) {
                return;
            }
            this->iovecs_vptr = os::allocate(sizeof(iovec)*buffer_count);
            if (this->iovecs_vptr == nullptr) {
                os::deallocate(this->ring_vptr);
                this->ring_vptr = nullptr;
                return;
            }
            auto* iovecs = static_cast<iovec*>(this->iovecs_vptr);
            for (U32 i = 0; i < buffer_count; i++) {
                iovecs[i].iov_base = this->buffers + i * buffer_size;
                iovecs[i].iov_len = buffer_size;
            }

            // setup io_uring
            auto* ring = static_cast<io_uring*>(this->ring_vptr);

            {
                io_uring_params params = {};
                int err = io_uring_queue_init_params(queue_depth, ring, &params);
                if (err < 0) {
                    os::deallocate(this->ring_vptr);
                    os::deallocate(this->iovecs_vptr);
                    this->ring_vptr = nullptr;
                    return;
                }
            }
            {
                madvise(this->buffers, this->buffer_size * this->buffer_count, MADV_WILLNEED);
                int err = io_uring_register_buffers(ring, iovecs, this->buffer_count);
                if (err < 0) {
                    io_uring_queue_exit(ring);
                    os::deallocate(this->ring_vptr);
                    os::deallocate(this->iovecs_vptr);
                    this->ring_vptr = nullptr;
                    return;
                }
            }

            // @todo @thread safe
            {
                get_ring_settings_internal()->available_buffer_bytes -= this->buffer_size * this->buffer_count;
                get_ring_settings_internal()->available_buffer_count -= this->buffer_count;
                get_ring_settings_internal()->available_ring_count -= 1;
                finalize_ring_settings(get_ring_settings_internal());
            }
        }

        Ring::~Ring() {
            if (this->ring_vptr != nullptr) {
                auto* ring = static_cast<io_uring*>(this->ring_vptr);

                int err = io_uring_unregister_buffers(ring);
                assert_true_always(err == 0, "io_uring_unregister_buffers failed");
                io_uring_queue_exit(ring);

                os::deallocate(this->ring_vptr);
                os::deallocate(this->iovecs_vptr);
                this->ring_vptr = nullptr;

                // @todo @thread safe
                {
                    get_ring_settings_internal()->available_buffer_bytes += this->buffer_size * this->buffer_count;
                    get_ring_settings_internal()->available_buffer_count += this->buffer_count;
                    get_ring_settings_internal()->available_ring_count += 1;
                    finalize_ring_settings(get_ring_settings_internal());
                }
            }
        }

        Ring::Ring(Ring&& other) noexcept 
            : server(other.server)
            , ring_vptr(other.ring_vptr)
            , buffers(other.buffers)
            , iovecs_vptr(other.iovecs_vptr)
            , buffer_size(other.buffer_size)
            , buffer_count(other.buffer_count) {
            other.server = os::zero_handle();
            other.ring_vptr = nullptr;
            other.buffers = nullptr;
            other.iovecs_vptr = nullptr;
            other.buffer_size = 0;
            other.buffer_count = 0;
        }

        Ring& Ring::operator=(Ring&& other) noexcept {
            if (this != &other) {
                if (this->ring_vptr) {
                    this->~Ring();
                }
                this->server = other.server;
                this->ring_vptr = other.ring_vptr;
                this->buffers = other.buffers;
                this->iovecs_vptr = other.iovecs_vptr;
                this->buffer_size = other.buffer_size;
                this->buffer_count = other.buffer_count;

                other.server = os::zero_handle();
                other.ring_vptr = nullptr;
                other.buffers = nullptr;
                other.iovecs_vptr = nullptr;
                other.buffer_size = 0;
                other.buffer_count = 0;
            }
            return *this;
        }

        Ring::operator bool() const {
            return this->ring_vptr != nullptr;
        }

        // ========================================================================
        // completion queue entry
        // ========================================================================

        U32 cqe_get_size(const Ring& ring) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            return io_uring_cq_ready(uring_ring);
        }

        static Error map_result_to_error_code(S32 res) {
            if (res >= 0) {
                return Error::None;
            }

            switch (res) {
                case -ENOENT: return Error::NotFound;
                case -EINVAL: return Error::Invalid;
                case -EIO:    return Error::IO;
                default:      return Error::Unknown;
            }
        }

        CQE cqe_top(Ring& ring) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_cqe* cqe = nullptr;
            
            if (io_uring_peek_cqe(uring_ring, &cqe) == 0 && cqe) {
                U64 user_data = (U64)io_uring_cqe_get_data(cqe);
                EventTypeTag type = decode_type(user_data);
                U64 data = decode_data(user_data);
                S32 result = cqe->res;
                Error error = map_result_to_error_code(result);

                switch (type) {
                    case TYPE_ACCEPT:{
                        return CQE{AcceptEvent{
                            .error = error,
                            .client = (error == Error::None) ? fd_to_handle(result) : os::zero_handle(),
                        }};
                    }break;
                    case TYPE_MULTISHOT_ACCEPT:{
                        return CQE{MultishotAcceptEvent{
                            .error = error,
                            .client = (error == Error::None) ? fd_to_handle(result) : os::zero_handle(),
                        }};
                    }break;
                    case TYPE_READ:{
                        assert_true(data < ring.buffer_count, "invalid cqe user data");
                        return CQE{ReadEvent{
                            .error = error,
                            .buffer_idx = static_cast<U32>(data),
                            .bytes_read = static_cast<U32>(max(result, 0_s32)),
                        }};
                    }break;
                    case TYPE_WRITE:{
                        assert_true(data < ring.buffer_count, "invalid cqe user data");
                        return CQE{WriteEvent{
                            .error = error,
                            .buffer_idx = static_cast<U32>(data),
                            .bytes_written = static_cast<U32>(max(result, 0_s32)),
                        }};
                    }break;
                    case TYPE_CLOSE:{
                        return CQE{CloseEvent{
                            .error = error,
                            .client = (error == Error::None) ? fd_to_handle(result) : os::zero_handle(),
                        }};
                    }break;
                    case TYPE_MASK:{}break;
                    case DATA_MASK:{}break;
                }
            }
            return CQE{};
        }

        bool cqe_pop(Ring& ring, U32 count) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            assert_true(count > 0, "count must be positive");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_cq_advance(uring_ring, count);
            return true;
        }

        // ========================================================================
        // submission queue entry
        // ========================================================================

        bool sqe_push_accept(Ring& ring) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_sqe* sqe = io_uring_get_sqe(uring_ring);
            if (sqe == nullptr) {
                return false;
            }

            int server_fd = handle_to_fd(ring.server);
            U64 user_data = encode_user_data(TYPE_ACCEPT, 0);
            
            io_uring_prep_accept(sqe, server_fd, nullptr, nullptr, 0);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        bool sqe_push_multishot_accept(Ring& ring) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_sqe* sqe = io_uring_get_sqe(uring_ring);
            if (sqe == nullptr) {
                return false;
            }

            int server_fd = handle_to_fd(ring.server);
            U64 user_data = encode_user_data(TYPE_MULTISHOT_ACCEPT, 0);
            
            io_uring_prep_multishot_accept(sqe, server_fd, nullptr, nullptr, 0);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        bool sqe_push_close(Ring& ring, os::Handle client) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_sqe* sqe = io_uring_get_sqe(uring_ring);
            if (sqe == nullptr) {
                return false;
            }

            int client_fd = handle_to_fd(client);
            U64 user_data = encode_user_data(TYPE_CLOSE, client_fd);
            
            io_uring_prep_close(sqe, client_fd);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        bool sqe_push_read(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            assert_true(buffer_idx < ring.buffer_count, "buffer_idx out of range");
            assert_true(byte_offset <= ring.buffer_size, "byte_offset out of range");
            assert_true(byte_offset + byte_count <= ring.buffer_size, "byte_count out of range");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_sqe* sqe = io_uring_get_sqe(uring_ring);
            if (sqe == nullptr) {
                return false;
            }

            int client_fd = handle_to_fd(client);
            U64 user_data = encode_user_data(TYPE_READ, buffer_idx);
            
            auto* iovecs = static_cast<iovec*>(ring.iovecs_vptr);
            io_uring_prep_read_fixed(sqe, client_fd, iovecs[buffer_idx].iov_base, byte_count, byte_offset, buffer_idx);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        bool sqe_push_write(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            assert_true(buffer_idx < ring.buffer_count, "buffer_idx out of range");
            assert_true(byte_offset <= ring.buffer_size, "byte_offset out of range");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            io_uring_sqe* sqe = io_uring_get_sqe(uring_ring);
            if (sqe == nullptr) {
                return false;
            }

            int client_fd = handle_to_fd(client);
            U64 user_data = encode_user_data(TYPE_WRITE, buffer_idx);
            
            auto* iovecs = static_cast<iovec*>(ring.iovecs_vptr);
            io_uring_prep_write_fixed(sqe, client_fd, iovecs[buffer_idx].iov_base, byte_count, byte_offset, buffer_idx);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        bool sqe_submit_non_blocking(Ring& ring) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            int ret = io_uring_submit(uring_ring);
            return ret >= 0;
        }

        // ========================================================================
        // stats
        // ========================================================================

        Stats get_stats(const Ring& ring) {
            assert_true(static_cast<bool>(ring), "invalid ring");
            auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
            Stats stats = {};
            stats.entries = uring_ring->sq.ring_entries;
            stats.queue_depth = stats.entries;
            return stats;
        }

    #else
        // ========================================================================
        // ring
        // ========================================================================

        Ring::Ring(os::Handle server, U32 queue_depth, U64 buffer_size, U32 buffer_count)
            : server(server)
            , buffers(nullptr)
            , buffer_size(buffer_size)
            , buffer_count(buffer_count) {}

        Ring::~Ring() {}

        Ring::Ring(Ring&& other) noexcept 
            : server(other.server)
            , ring_vptr(other.ring_vptr)
            , buffers(other.buffers)
            , iovecs_vptr(other.iovecs_vptr)
            , buffer_size(other.buffer_size)
            , buffer_count(other.buffer_count) {
            other.server = os::zero_handle();
            other.ring_vptr = nullptr;
            other.buffers = nullptr;
            other.iovecs_vptr = nullptr;
            other.buffer_size = 0;
            other.buffer_count = 0;
        }

        Ring& Ring::operator=(Ring&& other) noexcept {
            if (this != &other) {
                server = other.server;
                ring_vptr = other.ring_vptr;
                buffers = other.buffers;
                iovecs_vptr = other.iovecs_vptr;
                buffer_size = other.buffer_size;
                buffer_count = other.buffer_count;

                other.server = os::zero_handle();
                other.ring_vptr = nullptr;
                other.buffers = nullptr;
                other.iovecs_vptr = nullptr;
                other.buffer_size = 0;
                other.buffer_count = 0;
            }
            return *this;
        }

        Ring::operator bool() const {
            return false;
        }

        // ========================================================================
        // completion queue entry
        // ========================================================================

        U32 cqe_get_size(const Ring& ring) {
            assert_true_always(false, "io_uring not available on this platform");
            return 0;
        }

        CQE cqe_top(Ring& ring) {
            assert_true_always(false, "io_uring not available on this platform");
            return CQE{};
        }

        bool cqe_pop(Ring& ring, U32 count) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        // ========================================================================
        // submission queue entry
        // ========================================================================

        bool sqe_push_accept(Ring& ring) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        bool sqe_push_multishot_accept(Ring& ring) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        bool sqe_push_close(Ring& ring, os::Handle client) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        bool sqe_push_read(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        bool sqe_push_write(Ring& ring, os::Handle client, U32 buffer_idx, U32 byte_offset, U32 byte_count) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        bool sqe_submit_non_blocking(Ring& ring) {
            assert_true_always(false, "io_uring not available on this platform");
            return false;
        }

        // ========================================================================
        // stats
        // ========================================================================

        Stats get_stats(const Ring& ring) {
            assert_true_always(false, "io_uring not available on this platform");
            return Stats{};
        }

    #endif
}
