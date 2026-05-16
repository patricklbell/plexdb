module;
#include "macros.h"

#if PLEXDB_OS_LINUX
    #include <unistd.h>
    #include <signal.h>
    #include <stdlib.h>
    #include <sys/epoll.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

module plexdb.os.signal;

import plexdb.dynamic.containers;

namespace plexdb::os {
    #if PLEXDB_OS_LINUX
        static int handle_to_fd(Handle h) { return static_cast<int>(h.u32[0]); }
        static Handle fd_to_handle(int fd) { return Handle{.u32={static_cast<U32>(fd)}}; }

        Notifier::Notifier() {
            int fds[2];
            int res = pipe(fds);
            assert_true_always(res == 0, "failed to create signal notifier");

            this->read = fd_to_handle(fds[0]);
            this->write = fd_to_handle(fds[1]);
        }

        Notifier::~Notifier() {
            if (handle_to_fd(this->read) >= 0) {
                close(handle_to_fd(this->read));
            }
            if (handle_to_fd(this->write) >= 0) {
                close(handle_to_fd(this->write));
            }

            this->read  = zero_handle();
            this->write = zero_handle();
        }

        // @note write result intentionally ignored - safe to call from signal handlers
        void signal_notify_safe(Notifier& notifier) {
            char c = 0;
            ::write(handle_to_fd(notifier.write), &c, 1);
        }

        static void signal_register_linux(int signal_number, SignalHandler handler) {
            struct sigaction sa;
            memory_zero(&sa, sizeof(sa));
            sa.sa_handler = handler;
            ::sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            int res = ::sigaction(signal_number, &sa, nullptr);
            assert_true_always(res == 0, "sigaction failed");
        }

        void signal_register_kill(SignalHandler handler) {
            #ifdef PLEXDB_ENABLE_USER_KILL_SIGNAL
                // @note used by custom kill script
                signal_register_linux(SIGUSR1, handler);
            #else
                signal_register_linux(SIGINT, handler);
                signal_register_linux(SIGTERM, handler);
            #endif
        }

        void signal_ignore_reload() {
            signal_register_linux(SIGHUP, SIG_IGN);
        }

        void signal_register_stop(SignalHandler handler) {
            signal_register_linux(SIGSTOP, handler);
            signal_register_linux(SIGTSTP, handler);
        }

        void signal_ignore_pipe() {
            signal_register_linux(SIGPIPE, SIG_IGN);
        }

        static_assert(sizeof(pid_t) == sizeof(U32));
        static pid_t linux_handle_to_pid(Handle handle) { return memory_cast<S32>(&handle.u32[0]); }

        void signal_send_kill(Handle process) {
            ::kill(linux_handle_to_pid(process), SIGKILL);
        }

        Poll::Poll() {
            int fd = ::epoll_create1(0);
            if (fd == -1) {
                this->handle = zero_handle();
            } else {
                this->handle = fd_to_handle(fd);
            }
        }
        Poll::~Poll() {
            if (!is_zero_handle(this->handle)) {
                close(handle_to_fd(this->handle));
            }
        }

        Poll::Poll(Poll&& other) noexcept : handle(other.handle) {}
        Poll& Poll::operator=(Poll&& other) noexcept {
            if (this != &other) {
                if (!is_zero_handle(this->handle)) {
                    this->~Poll();
                }
                this->handle = other.handle;

                other.handle = zero_handle();
            }
            return *this;
        }

        Poll::operator bool() const {
            return !is_zero_handle(this->handle);
        }

        static U32 poll_mask_to_linux(PollEventMask events) {
            U32 out = 0;
            if ((U32(events) & U32(PollEventMask::Read)) != 0) out |= EPOLLIN;
            if ((U32(events) & U32(PollEventMask::Write)) != 0) out |= EPOLLOUT;
            if ((U32(events) & U32(PollEventMask::Error)) != 0) out |= EPOLLERR;
            if ((U32(events) & U32(PollEventMask::HangUp)) != 0) out |= EPOLLHUP;
            return out;
        }

        static PollEventMask linux_mask_to_poll(U32 events) {
            PollEventMask out = PollEventMask::None;
            if ((events & EPOLLIN) != 0)  out = out | PollEventMask::Read;
            if ((events & EPOLLOUT) != 0) out = out | PollEventMask::Write;
            if ((events & EPOLLERR) != 0) out = out | PollEventMask::Error;
            if ((events & EPOLLHUP) != 0) out = out | PollEventMask::HangUp;
            return out;
        }

        void poll_unblock_on(Poll& poll, const Notifier& notifier) {
            assert_true(static_cast<bool>(poll), "invalid notifier, cannot poll");
            poll_unblock_on(poll, notifier.read);
        }
        void poll_unblock_on(Poll& poll, const Handle& file_or_socket) {
            poll_unblock_on(
                poll,
                file_or_socket,
                PollEventMask::Read | PollEventMask::Error | PollEventMask::HangUp
            );
        }
        void poll_unblock_on(Poll& poll, const Handle& file_or_socket, PollEventMask events) {
            assert_true(!is_zero_handle(file_or_socket), "invalid notifier, cannot poll");
            struct ::epoll_event ev;
            ev.events = poll_mask_to_linux(events);
            ev.data.fd = handle_to_fd(file_or_socket);
            int res = ::epoll_ctl(handle_to_fd(poll.handle), EPOLL_CTL_ADD, handle_to_fd(file_or_socket), &ev);
            assert_true(res != -1, "epoll_ctl call failed while adding file to epoll");
        }

        void poll_update_mask(Poll& poll, const Handle& file_or_socket, PollEventMask events) {
            assert_true(static_cast<bool>(poll), "invalid poll, cannot update");
            assert_true(!is_zero_handle(file_or_socket), "invalid handle, cannot update poll");
            struct ::epoll_event ev;
            ev.events = poll_mask_to_linux(events);
            ev.data.fd = handle_to_fd(file_or_socket);
            int res = ::epoll_ctl(handle_to_fd(poll.handle), EPOLL_CTL_MOD, handle_to_fd(file_or_socket), &ev);
            assert_true(res != -1, "epoll_ctl call failed while updating file in epoll");
        }


        void poll_dont_unblock_on(Poll& poll, const Notifier& notifier) {
            assert_true(static_cast<bool>(poll), "invalid notifier, cannot poll");
            poll_dont_unblock_on(poll, notifier.read);
        }
        void poll_dont_unblock_on(Poll& poll, const Handle& file_or_socket) {
            assert_true(!is_zero_handle(file_or_socket), "invalid notifier, cannot remove from poll");

            struct ::epoll_event ev{};
            int res = ::epoll_ctl(
                handle_to_fd(poll.handle),
                EPOLL_CTL_DEL,
                handle_to_fd(file_or_socket),
                &ev
            );
            assert_true(res != -1 || errno == ENOENT, "epoll_ctl call failed while removing file from epoll");
        }

        bool block_until_poll_unblocks(const Poll& poll) {
            assert_true(static_cast<bool>(poll), "invalid poll, cannot wait");
            struct ::epoll_event events[1];
            S32 n = ::epoll_wait(handle_to_fd(poll.handle), events, 1, -1);
            return n > 0;
        }

        void drain_notifier(const Notifier& notifier) {
            int fd = handle_to_fd(notifier.read);
            int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            char buf[64];
            while (::read(fd, buf, sizeof(buf)) > 0) {}
            ::fcntl(fd, F_SETFL, flags); // restore
        }

        bool block_until_poll_unblocks_wth_events(const Poll& poll, CappedTArrayView<PollEvent>* out_events) {
            U64 max_out_events_count = out_events->length - out_events->cap;

            assert_true(static_cast<bool>(poll), "invalid poll, cannot wait");
            assert_true(out_events->ptr != nullptr && max_out_events_count > 0, "out_events must contain space");

            static thread_local DynamicArray<epoll_event> epoll_events_scratch;

            clear(epoll_events_scratch);
            resize(epoll_events_scratch, max_out_events_count);

            S32 n = ::epoll_wait(
                handle_to_fd(poll.handle),
                epoll_events_scratch.ptr,
                max_out_events_count,
                -1
            );

            assert_true(static_cast<U64>(max(n, 0)) <= max_out_events_count, "too many events from epoll_wait");
            for (S32 i = 0; i < n; ++i) {
                (*out_events)[out_events->cap++] = PollEvent{
                    .handle = fd_to_handle(epoll_events_scratch[i].data.fd),
                    .events = linux_mask_to_poll(epoll_events_scratch[i].events),
                };
            }

            return n > 0;
        }

    #else
        #error "Signal library not implemented for OS"
    #endif
}
