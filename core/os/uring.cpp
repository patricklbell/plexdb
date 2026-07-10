module;
#include <plexdb/macros/macros.h>

#if CORE_ENABLE_IO_URING
#if !PLEXDB_OS_LINUX
#error "Invalid build configuration, io_uring support is enabled but detected os was not linux"
#endif
#include <liburing.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
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
        TYPE_ACCEPT           = 0x1_u64 << 60,
        TYPE_READ             = 0x2_u64 << 60,
        TYPE_WRITE            = 0x3_u64 << 60,
        TYPE_CLOSE            = 0x4_u64 << 60,
        TYPE_MULTISHOT_ACCEPT = 0x5_u64 << 60,
        TYPE_FILE_READ        = 0x6_u64 << 60,
        TYPE_FILE_WRITE       = 0x7_u64 << 60,
        TYPE_FILE_SYNC        = 0x8_u64 << 60,

        TYPE_MASK = 0xF_u64 << 60,
        DATA_MASK = ~TYPE_MASK
    };

    [[maybe_unused]] static U64 encode_user_data(EventTypeTag type, U64 data) {
        return type | (data & DATA_MASK);
    }

    [[maybe_unused]] static EventTypeTag decode_type(U64 user_data) {
        return (EventTypeTag)(user_data & TYPE_MASK);
    }

    [[maybe_unused]] static U64 decode_data(U64 user_data) {
        return user_data & DATA_MASK;
    }

    Optional<RingSettings> g_ring_settings_opt;

    // Hard safety ceiling/floor for a single ring's registered-buffer footprint,
    // applied unconditionally regardless of measured RLIMIT_MEMLOCK/total_memory — a
    // generous or "unlimited" memlock limit must never translate into an unbounded
    // mlock request (registering tens of GB of buffers can pin enough physical
    // memory to bring the host down). Starting values follow common io_uring/reactor
    // practice (e.g. Seastar/ScyllaDB shard-local buffer pools, liburing benchmark
    // defaults): dozens-to-a-few-hundred in-flight buffers of tens of KB each is
    // enough to keep a NIC/NVMe queue full without registering more than a few MB of
    // locked memory per ring. Tune with tools, not by hand.
    constexpr U64 MIN_RING_BUFFER_SIZE  = 4_kb;
    constexpr U64 MAX_RING_BUFFER_SIZE  = 64_kb;
    constexpr U32 MIN_RING_BUFFER_COUNT = 8;
    constexpr U32 MAX_RING_BUFFER_COUNT = 256;
    constexpr U32 MIN_RING_QUEUE_DEPTH  = 8;
    constexpr U32 MAX_RING_QUEUE_DEPTH  = 256;

    // @todo tune from real workload data (or compute dynamically); 8 pages
    // is enough to coalesce several WAL frames (page + small frame header)
    // per registered buffer without over-committing pinned memory per buffer.
    constexpr U64 FILE_RING_BUFFER_PAGE_MULTIPLIER = 8;

    struct ClampedRingSize {
        U64  buffer_size;
        U32  buffer_count;
        U32  queue_depth;
        bool ok;
    };

    static ClampedRingSize clamp_ring_size(
        U64 available_bytes, U32 available_buffer_count_cap, U32 available_queue_depth,
        U64 page_size, U64 wanted_buffer_size, U32 wanted_buffer_count, U32 wanted_queue_depth
    ) {
        // @note buffer_size must be page-aligned for io_uring_register_buffers mlock accounting
        U64 buffer_size = align_down(
            min(max(wanted_buffer_size, MIN_RING_BUFFER_SIZE), MAX_RING_BUFFER_SIZE),
            max(page_size, 1_u64)
        );
        if (buffer_size == 0) {
            return {.buffer_size = 0, .buffer_count = 0, .queue_depth = 0, .ok = false};
        }

        U32 wanted_count_clamped = min(max(wanted_buffer_count, MIN_RING_BUFFER_COUNT), MAX_RING_BUFFER_COUNT);
        U32 buffer_count         = U32(min(
            U64(wanted_count_clamped),
            min(available_bytes / buffer_size, U64(available_buffer_count_cap))
        ));

        U32 queue_depth = min(max(wanted_queue_depth, MIN_RING_QUEUE_DEPTH), min(MAX_RING_QUEUE_DEPTH, available_queue_depth));

        bool ok = buffer_count >= MIN_RING_BUFFER_COUNT && queue_depth >= MIN_RING_QUEUE_DEPTH;
        return {
            .buffer_size  = buffer_size,
            .buffer_count = ok ? buffer_count : 0,
            .queue_depth  = ok ? queue_depth : 0,
            .ok           = ok,
        };
    }

    // @warn re-run after allocations, since a ring being constructed or destroyed changes global budget
    static bool ring_settings_available(const RingSettings* settings) {
        return os::get_kernel_features()->io_uring.supported
            && settings->available_ring_count > 0
            && clamp_ring_size(
                   settings->available_buffer_bytes, settings->available_buffer_count,
                   settings->available_queue_depth, os::get_system_info()->page_size,
                   MIN_RING_BUFFER_SIZE, MIN_RING_BUFFER_COUNT, MIN_RING_QUEUE_DEPTH
            )
                   .ok;
    }

    static RingSettings* get_ring_settings_internal() {
        if (!g_ring_settings_opt) {
            // @note reserve 64KB for io_uring ring structures (SQ/CQ rings, kernel overhead)
            constexpr U64 IO_URING_OVERHEAD_BYTES = 64_kb;
            U64           mlock_limit             = os::get_system_info()->mlock_limit;
            U64           usable_mlock            = max(mlock_limit, IO_URING_OVERHEAD_BYTES) - IO_URING_OVERHEAD_BYTES;

            RingSettings ring_settings = {
                .available             = false,
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
                .available_ring_count   = os::get_system_info()->vma_limit,
            };

            ring_settings.available = ring_settings_available(&ring_settings);

            g_ring_settings_opt.emplace(move(ring_settings));
        }

        return &(*g_ring_settings_opt);
    }

    const RingSettings* get_ring_settings() {
        return get_ring_settings_internal();
    }

    U8* get_buffer_ptr(Ring& ring, U32 buffer_idx) {
        return &ring.buffers[buffer_idx * ring.buffer_size];
    }

    NetworkIOBudget compute_network_io_budget(IOBudgetOverride ring_override) {
        RingSettings* settings = get_ring_settings_internal();
        if (!settings->available) {
            return NetworkIOBudget{.queue_depth = 0, .buffer_size = 0, .buffer_count = 0};
        }

        auto* kf             = os::get_kernel_features();
        U32   max_sq_entries = kf->io_uring.supported ? min(kf->io_uring.max_sq_entries, kf->io_uring.max_cq_entries) : 0;

        U64 wanted_buffer_size  = ring_override.buffer_size ? *ring_override.buffer_size : 64_kb;
        U32 wanted_buffer_count = ring_override.buffer_count ? *ring_override.buffer_count : 64;
        U32 wanted_queue_depth  = ring_override.queue_depth ? *ring_override.queue_depth : (max_sq_entries > 0 ? max_sq_entries / 2 : 32);

        ClampedRingSize clamped = clamp_ring_size(
            settings->available_buffer_bytes, settings->available_buffer_count, settings->available_queue_depth,
            os::get_system_info()->page_size, wanted_buffer_size, wanted_buffer_count, wanted_queue_depth
        );

        return NetworkIOBudget{.queue_depth = clamped.queue_depth, .buffer_size = clamped.buffer_size, .buffer_count = clamped.buffer_count};
    }

    FileIOBudget compute_file_io_budget(U64 page_size, IOBudgetOverride ring_override) {
        RingSettings* settings = get_ring_settings_internal();
        if (!settings->available) {
            return FileIOBudget{.queue_depth = 0, .buffer_size = 0, .buffer_count = 0};
        }

        U64 wanted_buffer_size  = ring_override.buffer_size ? *ring_override.buffer_size : page_size * FILE_RING_BUFFER_PAGE_MULTIPLIER;
        U32 wanted_buffer_count = ring_override.buffer_count ? *ring_override.buffer_count : 64;
        U32 wanted_queue_depth  = ring_override.queue_depth ? *ring_override.queue_depth : os::get_system_info()->disk_queue_depth;

        ClampedRingSize clamped = clamp_ring_size(
            settings->available_buffer_bytes, settings->available_buffer_count, settings->available_queue_depth,
            page_size, wanted_buffer_size, wanted_buffer_count, wanted_queue_depth
        );

        return FileIOBudget{.queue_depth = clamped.queue_depth, .buffer_size = clamped.buffer_size, .buffer_count = clamped.buffer_count};
    }

#if CORE_ENABLE_IO_URING
    static int handle_to_fd(os::Handle h) {
        return static_cast<int>(h.u32[0]);
    }
    static os::Handle fd_to_handle(int fd) {
        assert_true(fd >= 0, "invalid fd");
        return os::Handle{.u32 = {static_cast<U32>(fd)}};
    }

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
        int err       = posix_memalign((void**)&this->buffers, page_size, buffer_size * buffer_count);
        if (err != 0) {
            return;
        }
        this->ring_vptr = os::allocate_zero(sizeof(io_uring));
        if (this->ring_vptr == nullptr) {
            return;
        }
        this->iovecs_vptr = os::allocate(sizeof(iovec) * buffer_count);
        if (this->iovecs_vptr == nullptr) {
            os::deallocate(this->ring_vptr);
            this->ring_vptr = nullptr;
            return;
        }
        auto* iovecs = static_cast<iovec*>(this->iovecs_vptr);
        for (U32 i = 0; i < buffer_count; i++) {
            iovecs[i].iov_base = this->buffers + i * buffer_size;
            iovecs[i].iov_len  = buffer_size;
        }

        // setup io_uring
        auto* ring = static_cast<io_uring*>(this->ring_vptr);

        {
            io_uring_params params = {};
            int             err    = io_uring_queue_init_params(queue_depth, ring, &params);
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
        {
            int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (efd >= 0) {
                if (io_uring_register_eventfd(ring, efd) == 0) {
                    this->event_fd = fd_to_handle(efd);
                } else {
                    ::close(efd);
                }
            }
        }

        // @todo @thread safe
        {
            RingSettings* settings = get_ring_settings_internal();
            settings->available_buffer_bytes -= this->buffer_size * this->buffer_count;
            settings->available_ring_count -= 1;
            settings->available = ring_settings_available(settings);
        }
    }

    Ring::~Ring() {
        if (this->ring_vptr != nullptr) {
            auto* ring = static_cast<io_uring*>(this->ring_vptr);

            if (!os::is_zero_handle(this->event_fd)) {
                io_uring_unregister_eventfd(ring);
                ::close(handle_to_fd(this->event_fd));
                this->event_fd = os::zero_handle();
            }

            int err = io_uring_unregister_buffers(ring);
            assert_true_always(err == 0, "io_uring_unregister_buffers failed");
            io_uring_queue_exit(ring);

            os::deallocate(this->ring_vptr);
            os::deallocate(this->iovecs_vptr);
            os::deallocate(this->buffers);
            this->ring_vptr = nullptr;
            this->buffers   = nullptr;

            // @todo @thread safe
            {
                RingSettings* settings = get_ring_settings_internal();
                settings->available_buffer_bytes += this->buffer_size * this->buffer_count;
                settings->available_ring_count += 1;
                settings->available = ring_settings_available(settings);
            }
        }
    }

    Ring::Ring(Ring&& other) noexcept
        : server(other.server)
        , event_fd(other.event_fd)
        , ring_vptr(other.ring_vptr)
        , buffers(other.buffers)
        , iovecs_vptr(other.iovecs_vptr)
        , buffer_size(other.buffer_size)
        , buffer_count(other.buffer_count) {
        other.server       = os::zero_handle();
        other.event_fd     = os::zero_handle();
        other.ring_vptr    = nullptr;
        other.buffers      = nullptr;
        other.iovecs_vptr  = nullptr;
        other.buffer_size  = 0;
        other.buffer_count = 0;
    }

    Ring& Ring::operator=(Ring&& other) noexcept {
        if (this != &other) {
            if (this->ring_vptr) {
                this->~Ring();
            }
            this->server       = other.server;
            this->event_fd     = other.event_fd;
            this->ring_vptr    = other.ring_vptr;
            this->buffers      = other.buffers;
            this->iovecs_vptr  = other.iovecs_vptr;
            this->buffer_size  = other.buffer_size;
            this->buffer_count = other.buffer_count;

            other.server       = os::zero_handle();
            other.event_fd     = os::zero_handle();
            other.ring_vptr    = nullptr;
            other.buffers      = nullptr;
            other.iovecs_vptr  = nullptr;
            other.buffer_size  = 0;
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
            case -ENOENT:
                return Error::NotFound;
            case -EINVAL:
                return Error::Invalid;
            case -EIO:
                return Error::IO;
            default:
                return Error::Unknown;
        }
    }

    CQE cqe_top(Ring& ring) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_cqe* cqe        = nullptr;

        if (io_uring_peek_cqe(uring_ring, &cqe) == 0 && cqe) {
            U64          user_data = (U64)io_uring_cqe_get_data(cqe);
            EventTypeTag type      = decode_type(user_data);
            U64          data      = decode_data(user_data);
            S32          result    = cqe->res;
            Error        error     = map_result_to_error_code(result);

            switch (type) {
                case TYPE_ACCEPT: {
                    return CQE{
                        AcceptEvent{
                                    .error  = error,
                                    .client = (error == Error::None) ? fd_to_handle(result) : os::zero_handle(),
                                    }
                    };
                } break;
                case TYPE_MULTISHOT_ACCEPT: {
                    return CQE{
                        MultishotAcceptEvent{
                                             .error  = error,
                                             .client = (error == Error::None) ? fd_to_handle(result) : os::zero_handle(),
                                             }
                    };
                } break;
                case TYPE_READ: {
                    assert_true(data < ring.buffer_count, "invalid cqe user data");
                    return CQE{
                        ReadEvent{
                                  .error      = error,
                                  .buffer_idx = static_cast<U32>(data),
                                  .bytes_read = static_cast<U32>(max(result, 0_s32)),
                                  }
                    };
                } break;
                case TYPE_WRITE: {
                    assert_true(data < ring.buffer_count, "invalid cqe user data");
                    return CQE{
                        WriteEvent{
                                   .error         = error,
                                   .buffer_idx    = static_cast<U32>(data),
                                   .bytes_written = static_cast<U32>(max(result, 0_s32)),
                                   }
                    };
                } break;
                case TYPE_CLOSE: {
                    return CQE{
                        CloseEvent{
                                   .error  = error,
                                   .client = (error == Error::None) ? fd_to_handle(static_cast<int>(data)) : os::zero_handle(),
                                   }
                    };
                } break;
                case TYPE_FILE_READ: {
                    return CQE{
                        FileReadEvent{
                                      .error      = error,
                                      .buffer_idx = static_cast<U32>(data & 0xFFFFFFFF),
                                      .bytes_read = static_cast<U32>(max(result, 0_s32)),
                                      .token      = data >> 32,
                                      }
                    };
                } break;
                case TYPE_FILE_WRITE: {
                    return CQE{
                        FileWriteEvent{
                                       .error         = error,
                                       .buffer_idx    = static_cast<U32>(data & 0xFFFFFFFF),
                                       .bytes_written = static_cast<U32>(max(result, 0_s32)),
                                       .token         = data >> 32,
                                       }
                    };
                } break;
                case TYPE_FILE_SYNC: {
                    return CQE{
                        FileSyncEvent{
                                      .error = error,
                                      .token = data,
                                      }
                    };
                } break;
                case TYPE_MASK: {
                } break;
                case DATA_MASK: {
                } break;
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

    // @note io_uring_get_sqe fails only when the SQ ring is full; submitting
    // frees every prepped-but-unsubmitted slot, so retry once after a flush.
    static io_uring_sqe* get_sqe_or_submit(io_uring* uring_ring) {
        io_uring_sqe* sqe = io_uring_get_sqe(uring_ring);
        if (sqe == nullptr) {
            io_uring_submit(uring_ring);
            sqe = io_uring_get_sqe(uring_ring);
        }
        return sqe;
    }

    bool sqe_push_accept(Ring& ring) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
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
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
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
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
        if (sqe == nullptr) {
            return false;
        }

        int client_fd = handle_to_fd(client);
        assert_true(client_fd >= 0, "invalid fd, cannot be encoded in io_uring unsigned user data");
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
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
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
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
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

    bool sqe_push_file_read(Ring& ring, os::Handle file, U32 buffer_idx, U64 file_offset, U32 count, U64 token) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        assert_true(buffer_idx < ring.buffer_count, "buffer_idx out of range");
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
        if (sqe == nullptr) {
            return false;
        }

        int file_fd   = handle_to_fd(file);
        U64 user_data = encode_user_data(TYPE_FILE_READ, (token << 32) | U64(buffer_idx));

        auto* iovecs = static_cast<iovec*>(ring.iovecs_vptr);
        io_uring_prep_read_fixed(sqe, file_fd, iovecs[buffer_idx].iov_base, count, file_offset, buffer_idx);
        io_uring_sqe_set_data(sqe, (void*)user_data);
        return true;
    }

    bool sqe_push_file_write(Ring& ring, os::Handle file, U32 buffer_idx, U64 file_offset, U32 count, U64 token) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        assert_true(buffer_idx < ring.buffer_count, "buffer_idx out of range");
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
        if (sqe == nullptr) {
            return false;
        }

        int file_fd   = handle_to_fd(file);
        U64 user_data = encode_user_data(TYPE_FILE_WRITE, (token << 32) | U64(buffer_idx));

        auto* iovecs = static_cast<iovec*>(ring.iovecs_vptr);
        io_uring_prep_write_fixed(sqe, file_fd, iovecs[buffer_idx].iov_base, count, file_offset, buffer_idx);
        io_uring_sqe_set_data(sqe, (void*)user_data);
        return true;
    }

    bool sqe_push_file_sync(Ring& ring, os::Handle file, U64 token) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        auto*         uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        io_uring_sqe* sqe        = get_sqe_or_submit(uring_ring);
        if (sqe == nullptr) {
            return false;
        }

        int file_fd   = handle_to_fd(file);
        U64 user_data = encode_user_data(TYPE_FILE_SYNC, token);

        io_uring_prep_fsync(sqe, file_fd, IORING_FSYNC_DATASYNC);
        io_uring_sqe_set_data(sqe, (void*)user_data);
        return true;
    }

    bool sqe_submit_non_blocking(Ring& ring) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        auto* uring_ring = static_cast<io_uring*>(ring.ring_vptr);
        int   ret        = io_uring_submit(uring_ring);
        return ret >= 0;
    }

    // ========================================================================
    // stats
    // ========================================================================

    Stats get_stats(const Ring& ring) {
        assert_true(static_cast<bool>(ring), "invalid ring");
        auto* uring_ring  = static_cast<io_uring*>(ring.ring_vptr);
        Stats stats       = {};
        stats.entries     = uring_ring->sq.ring_entries;
        stats.queue_depth = stats.entries;
        return stats;
    }

    bool ring_drain_event_fd(Ring& ring) {
        if (os::is_zero_handle(ring.event_fd)) {
            return false;
        }
        U64     val = 0;
        ssize_t r   = ::read(handle_to_fd(ring.event_fd), &val, sizeof(val));
        return r == static_cast<ssize_t>(sizeof(val));
    }

#else
    // ========================================================================
    // ring
    // ========================================================================

    Ring::Ring(os::Handle server, [[maybe_unused]] U32 queue_depth, U64 buffer_size, U32 buffer_count)
        : server(server)
        , buffers(nullptr)
        , buffer_size(buffer_size)
        , buffer_count(buffer_count) {
    }

    Ring::~Ring() {
    }

    Ring::Ring(Ring&& other) noexcept
        : server(other.server)
        , event_fd(other.event_fd)
        , ring_vptr(other.ring_vptr)
        , buffers(other.buffers)
        , iovecs_vptr(other.iovecs_vptr)
        , buffer_size(other.buffer_size)
        , buffer_count(other.buffer_count) {
        other.server       = os::zero_handle();
        other.event_fd     = os::zero_handle();
        other.ring_vptr    = nullptr;
        other.buffers      = nullptr;
        other.iovecs_vptr  = nullptr;
        other.buffer_size  = 0;
        other.buffer_count = 0;
    }

    Ring& Ring::operator=(Ring&& other) noexcept {
        if (this != &other) {
            server       = other.server;
            event_fd     = other.event_fd;
            ring_vptr    = other.ring_vptr;
            buffers      = other.buffers;
            iovecs_vptr  = other.iovecs_vptr;
            buffer_size  = other.buffer_size;
            buffer_count = other.buffer_count;

            other.server       = os::zero_handle();
            other.event_fd     = os::zero_handle();
            other.ring_vptr    = nullptr;
            other.buffers      = nullptr;
            other.iovecs_vptr  = nullptr;
            other.buffer_size  = 0;
            other.buffer_count = 0;
        }
        return *this;
    }

    Ring::operator bool() const {
        return false;
    }

    bool ring_drain_event_fd([[maybe_unused]] Ring& ring) {
        return false;
    }

    // ========================================================================
    // completion queue entry
    // ========================================================================

    U32 cqe_get_size([[maybe_unused]] const Ring& ring) {
        assert_true_always(false, "io_uring not available on this platform");
        return 0;
    }

    CQE cqe_top([[maybe_unused]] Ring& ring) {
        assert_true_always(false, "io_uring not available on this platform");
        return CQE{};
    }

    bool cqe_pop([[maybe_unused]] Ring& ring, [[maybe_unused]] U32 count) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    // ========================================================================
    // submission queue entry
    // ========================================================================

    bool sqe_push_accept([[maybe_unused]] Ring& ring) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_multishot_accept([[maybe_unused]] Ring& ring) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_close([[maybe_unused]] Ring& ring, [[maybe_unused]] os::Handle client) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_read([[maybe_unused]] Ring& ring, [[maybe_unused]] os::Handle client, [[maybe_unused]] U32 buffer_idx, [[maybe_unused]] U32 byte_offset, [[maybe_unused]] U32 byte_count) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_write([[maybe_unused]] Ring& ring, [[maybe_unused]] os::Handle client, [[maybe_unused]] U32 buffer_idx, [[maybe_unused]] U32 byte_offset, [[maybe_unused]] U32 byte_count) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_file_read([[maybe_unused]] Ring& ring, [[maybe_unused]] os::Handle file, [[maybe_unused]] U32 buffer_idx, [[maybe_unused]] U64 file_offset, [[maybe_unused]] U32 count, [[maybe_unused]] U64 token) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_file_write([[maybe_unused]] Ring& ring, [[maybe_unused]] os::Handle file, [[maybe_unused]] U32 buffer_idx, [[maybe_unused]] U64 file_offset, [[maybe_unused]] U32 count, [[maybe_unused]] U64 token) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_push_file_sync([[maybe_unused]] Ring& ring, [[maybe_unused]] os::Handle file, [[maybe_unused]] U64 token) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    bool sqe_submit_non_blocking([[maybe_unused]] Ring& ring) {
        assert_true_always(false, "io_uring not available on this platform");
        return false;
    }

    // ========================================================================
    // stats
    // ========================================================================

    Stats get_stats([[maybe_unused]] const Ring& ring) {
        assert_true_always(false, "io_uring not available on this platform");
        return Stats{};
    }

#endif
}
