module;
#include "macros.h"

#if PLEXDB_OS_LINUX
    #include <sys/epoll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <errno.h>
    #include <stdlib.h>
    #if PLEXDB_HAS_IO_URING
        #include <atomic>
        #include <liburing.h>
    #endif
#endif

module plexdb.os.event_loop;

import plexdb.os.sysinfo;

namespace plexdb::os {
    #if PLEXDB_OS_LINUX

    // ========================================================================
    // io_uring backend
    // ========================================================================
    #if PLEXDB_HAS_IO_URING
    namespace uring {
        static void init(EventLoop& loop, U32 queue_depth) {
            io_uring_params params{};
            params.sq_thread_idle = 2000;
            params.cq_entries = queue_depth * 2;

            int res = io_uring_queue_init_params(queue_depth, &loop.ring, &params);
            assert_true_always(res == 0, "io_uring_queue_init_params failed");
        }

        static void destroy(EventLoop& loop) {
            io_uring_queue_exit(&loop.ring);
        }

        static bool register_buffers(EventLoop& loop, IoBuffer* buffers, int count) {
            iovec* iovecs = (iovec*)malloc(sizeof(iovec) * count);
            for (int i = 0; i < count; i++) {
                iovecs[i].iov_base = buffers[i].base;
                iovecs[i].iov_len = buffers[i].length;
            }
            int err = io_uring_register_buffers(&loop.ring, iovecs, count);
            free(iovecs);
            return err == 0;
        }

        static void unregister_buffers(EventLoop& loop) {
            io_uring_unregister_buffers(&loop.ring);
        }

        static bool submit_accept(EventLoop& loop, int listen_fd, U64 user_data) {
            io_uring_sqe* sqe = io_uring_get_sqe(&loop.ring);
            if (!sqe) return false;
            io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        static bool submit_multishot_accept(EventLoop& loop, int listen_fd, U64 user_data) {
            io_uring_sqe* sqe = io_uring_get_sqe(&loop.ring);
            if (!sqe) return false;
            io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        static bool submit_read(EventLoop& loop, int fd, void* buf, U64 len, int buf_index, U64 user_data) {
            io_uring_sqe* sqe = io_uring_get_sqe(&loop.ring);
            if (!sqe) return false;
            io_uring_prep_read_fixed(sqe, fd, buf, len, 0, buf_index);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        static bool submit_write(EventLoop& loop, int fd, const void* buf, U64 len, int buf_index, U64 user_data) {
            io_uring_sqe* sqe = io_uring_get_sqe(&loop.ring);
            if (!sqe) return false;
            io_uring_prep_write_fixed(sqe, fd, buf, len, 0, buf_index);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        static bool submit_close(EventLoop& loop, int fd, U64 user_data) {
            io_uring_sqe* sqe = io_uring_get_sqe(&loop.ring);
            if (!sqe) return false;
            io_uring_prep_close(sqe, fd);
            io_uring_sqe_set_data(sqe, (void*)user_data);
            return true;
        }

        static int flush(EventLoop& loop) {
            return io_uring_submit(&loop.ring);
        }

        static int drain(EventLoop& loop, CompletionEvent* events, int max_events) {
            int count = 0;
            io_uring_cqe* cqe;

            while (count < max_events) {
                int ret = io_uring_peek_cqe(&loop.ring, &cqe);
                if (ret != 0 || !cqe) break;

                events[count].result = cqe->res;
                events[count].user_data = (U64)io_uring_cqe_get_data(cqe);
                io_uring_cqe_seen(&loop.ring, cqe);
                count++;
            }

            return count;
        }

        static void init_epoll(EventLoop& loop) {
            loop.epoll_fd = epoll_create1(0);
            assert_true_always(loop.epoll_fd != -1, "failed to create epoll for event loop");

            {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = loop.signal_fd;
                int res = epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, loop.signal_fd, &ev);
                assert_true_always(res != -1, "epoll_ctl failed to add signal fd");
            }

            {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = loop.ring.ring_fd;
                int res = epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, loop.ring.ring_fd, &ev);
                assert_true_always(res != -1, "epoll_ctl failed to add ring fd");
            }
        }

        static void wait(EventLoop& loop) {
            struct epoll_event event;
            epoll_wait(loop.epoll_fd, &event, 1, -1);
        }
    }
    #endif

    // ========================================================================
    // epoll fallback backend
    //
    // @note completion-based interface over readiness-based epoll.
    // Operations are submitted as pending and executed when fds become ready.
    // ========================================================================
    namespace epoll_backend {
        // @note small fixed cap for pending operations, enough for typical server workloads
        constexpr int MAX_PENDING = 16384;

        enum class OpType : U8 { None, Accept, MultishotAccept, Read, Write, Close };

        struct PendingOp {
            OpType type    = OpType::None;
            int fd         = -1;
            void* buf      = nullptr;
            U64 len        = 0;
            U64 user_data  = 0;
        };
    }

    struct EpollState {
        epoll_backend::PendingOp pending[epoll_backend::MAX_PENDING];
        int pending_count = 0;

        CompletionEvent completions[EVENT_LOOP_MAX_DRAIN];
        int completion_count = 0;
        int completion_read  = 0;
    };

    namespace epoll_backend {
        static EpollState* alloc_state() {
            EpollState* s = (EpollState*)malloc(sizeof(EpollState));
            s->pending_count = 0;
            s->completion_count = 0;
            s->completion_read = 0;
            return s;
        }

        static void free_state(EpollState* s) {
            if (s) free(s);
        }

        static void queue_completion(EpollState* s, int result, U64 user_data) {
            if (s->completion_count < EVENT_LOOP_MAX_DRAIN) {
                s->completions[s->completion_count].result = result;
                s->completions[s->completion_count].user_data = user_data;
                s->completion_count++;
            }
        }

        static void init(EventLoop& loop) {
            loop.epoll_fd = epoll_create1(0);
            assert_true_always(loop.epoll_fd != -1, "failed to create epoll");

            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = loop.signal_fd;
            int res = epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, loop.signal_fd, &ev);
            assert_true_always(res != -1, "epoll_ctl failed to add signal fd");

            loop.epoll_state = alloc_state();
        }

        static void destroy(EventLoop& loop) {
            if (loop.epoll_fd >= 0) close(loop.epoll_fd);
            free_state(loop.epoll_state);
            loop.epoll_state = nullptr;
        }

        static bool add_pending(EventLoop& loop, int fd, OpType type, void* buf, U64 len, U64 user_data) {
            EpollState* s = loop.epoll_state;
            if (s->pending_count >= MAX_PENDING) return false;

            PendingOp& op = s->pending[s->pending_count++];
            op.type = type;
            op.fd = fd;
            op.buf = buf;
            op.len = len;
            op.user_data = user_data;
            return true;
        }

        static void remove_pending(EpollState* s, int idx) {
            s->pending[idx] = s->pending[s->pending_count - 1];
            s->pending_count--;
        }

        static bool submit_accept(EventLoop& loop, int listen_fd, U64 user_data) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = listen_fd;
            epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
            return add_pending(loop, listen_fd, OpType::Accept, nullptr, 0, user_data);
        }

        static bool submit_multishot_accept(EventLoop& loop, int listen_fd, U64 user_data) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = listen_fd;
            epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
            return add_pending(loop, listen_fd, OpType::MultishotAccept, nullptr, 0, user_data);
        }

        static bool submit_read(EventLoop& loop, int fd, void* buf, U64 len, U64 user_data) {
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLONESHOT;
            ev.data.fd = fd;
            // try MOD first, then ADD (fd may already be in epoll)
            if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
                epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
            }
            return add_pending(loop, fd, OpType::Read, buf, len, user_data);
        }

        static bool submit_write(EventLoop& loop, int fd, const void* buf, U64 len, U64 user_data) {
            struct epoll_event ev;
            ev.events = EPOLLOUT | EPOLLONESHOT;
            ev.data.fd = fd;
            if (epoll_ctl(loop.epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
                epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
            }
            return add_pending(loop, fd, OpType::Write, (void*)buf, len, user_data);
        }

        static bool submit_close(EventLoop& loop, int fd, U64 user_data) {
            epoll_ctl(loop.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            int res = close(fd);
            EpollState* s = loop.epoll_state;
            queue_completion(s, res == 0 ? 0 : -errno, user_data);
            return true;
        }

        static void process_ready_events(EventLoop& loop, struct epoll_event* epoll_events, int n) {
            EpollState* s = loop.epoll_state;

            for (int e = 0; e < n; e++) {
                int ready_fd = epoll_events[e].data.fd;
                if (ready_fd == loop.signal_fd) continue;

                for (int i = 0; i < s->pending_count; i++) {
                    PendingOp& op = s->pending[i];
                    if (op.fd != ready_fd) continue;

                    switch (op.type) {
                        case OpType::Accept: {
                            int client_fd = accept(ready_fd, nullptr, nullptr);
                            queue_completion(s, client_fd < 0 ? -errno : client_fd, op.user_data);
                            remove_pending(s, i);
                            i--;
                        } break;
                        case OpType::MultishotAccept: {
                            int client_fd = accept(ready_fd, nullptr, nullptr);
                            queue_completion(s, client_fd < 0 ? -errno : client_fd, op.user_data);
                            // @note multishot stays active
                        } break;
                        case OpType::Read: {
                            ssize_t bytes = read(ready_fd, op.buf, op.len);
                            queue_completion(s, bytes < 0 ? -errno : (int)bytes, op.user_data);
                            remove_pending(s, i);
                            i--;
                        } break;
                        case OpType::Write: {
                            ssize_t bytes = write(ready_fd, op.buf, op.len);
                            queue_completion(s, bytes < 0 ? -errno : (int)bytes, op.user_data);
                            remove_pending(s, i);
                            i--;
                        } break;
                        case OpType::Close:
                        case OpType::None:
                            break;
                    }
                }
            }
        }

        static int drain(EventLoop& loop, CompletionEvent* events, int max_events) {
            EpollState* s = loop.epoll_state;

            // first return any buffered completions
            int count = 0;
            while (s->completion_read < s->completion_count && count < max_events) {
                events[count++] = s->completions[s->completion_read++];
            }
            if (s->completion_read >= s->completion_count) {
                s->completion_count = 0;
                s->completion_read = 0;
            }
            if (count > 0) return count;

            // poll for new events
            struct epoll_event epoll_events[64];
            int n = epoll_wait(loop.epoll_fd, epoll_events, 64, 0);
            if (n > 0) {
                process_ready_events(loop, epoll_events, n);

                while (s->completion_read < s->completion_count && count < max_events) {
                    events[count++] = s->completions[s->completion_read++];
                }
                if (s->completion_read >= s->completion_count) {
                    s->completion_count = 0;
                    s->completion_read = 0;
                }
            }

            return count;
        }

        static void wait(EventLoop& loop) {
            struct epoll_event event;
            epoll_wait(loop.epoll_fd, &event, 1, -1);
        }
    }

    // ========================================================================
    // public interface - dispatches to appropriate backend
    // ========================================================================
    EventLoop event_loop_create(U32 queue_depth, int signal_fd) {
        EventLoop loop{};
        loop.signal_fd = signal_fd;
        loop.epoll_fd = -1;
        loop.epoll_state = nullptr;

        #if PLEXDB_HAS_IO_URING
        {
            const KernelFeatures* k = get_kernel_features();
            if (k->io_uring.supported) {
                loop.backend = EventLoop::Backend::IoUring;
                uring::init(loop, queue_depth);
                uring::init_epoll(loop);
                return loop;
            }
        }
        #endif

        loop.backend = EventLoop::Backend::Epoll;
        epoll_backend::init(loop);
        return loop;
    }

    void event_loop_destroy(EventLoop& loop) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            if (loop.epoll_fd >= 0) close(loop.epoll_fd);
            uring::destroy(loop);
            return;
        }
        #endif
        epoll_backend::destroy(loop);
    }

    bool event_loop_register_buffers(EventLoop& loop, IoBuffer* buffers, int count) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::register_buffers(loop, buffers, count);
        }
        #endif
        return true;
    }

    void event_loop_unregister_buffers(EventLoop& loop) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            uring::unregister_buffers(loop);
        }
        #endif
    }

    bool event_loop_submit_accept(EventLoop& loop, int listen_fd, U64 user_data) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::submit_accept(loop, listen_fd, user_data);
        }
        #endif
        return epoll_backend::submit_accept(loop, listen_fd, user_data);
    }

    bool event_loop_submit_multishot_accept(EventLoop& loop, int listen_fd, U64 user_data) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::submit_multishot_accept(loop, listen_fd, user_data);
        }
        #endif
        return epoll_backend::submit_multishot_accept(loop, listen_fd, user_data);
    }

    bool event_loop_submit_read(EventLoop& loop, int fd, void* buf, U64 len, int buf_index, U64 user_data) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::submit_read(loop, fd, buf, len, buf_index, user_data);
        }
        #endif
        return epoll_backend::submit_read(loop, fd, buf, len, user_data);
    }

    bool event_loop_submit_write(EventLoop& loop, int fd, const void* buf, U64 len, int buf_index, U64 user_data) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::submit_write(loop, fd, buf, len, buf_index, user_data);
        }
        #endif
        return epoll_backend::submit_write(loop, fd, buf, len, user_data);
    }

    bool event_loop_submit_close(EventLoop& loop, int fd, U64 user_data) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::submit_close(loop, fd, user_data);
        }
        #endif
        return epoll_backend::submit_close(loop, fd, user_data);
    }

    int event_loop_flush(EventLoop& loop) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::flush(loop);
        }
        #endif
        return 0;
    }

    int event_loop_drain(EventLoop& loop, CompletionEvent* events, int max_events) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            return uring::drain(loop, events, max_events);
        }
        #endif
        return epoll_backend::drain(loop, events, max_events);
    }

    void event_loop_wait(EventLoop& loop) {
        #if PLEXDB_HAS_IO_URING
        if (loop.backend == EventLoop::Backend::IoUring) {
            uring::wait(loop);
            return;
        }
        #endif
        epoll_backend::wait(loop);
    }

    #else
        #error "Event loop not implemented for OS"
    #endif
}
