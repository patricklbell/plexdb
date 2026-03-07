module;
#include "macros.h"

#if PLEXDB_OS_LINUX
    #include <unistd.h>
    #include <signal.h>
    #include <stdlib.h>
    #include <sys/epoll.h>
#endif

module plexdb.os.signal;

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
            #if PLEXDB_DEBUG
                // @todo detect debugger
                // @note used by custom kill script
                signal_register_linux(SIGUSR1, handler);
            #else
                // @todo investigate how gdb interferes with these
                signal_register_linux(SIGHUP, handler);
                signal_register_linux(SIGINT, handler);
                signal_register_linux(SIGTERM, handler);
            #endif
        }

        void signal_register_stop(SignalHandler handler) {
            signal_register_linux(SIGSTOP, handler);
            signal_register_linux(SIGTSTP, handler);
        }

        void signal_ignore_pipe() {
            signal_register_linux(SIGPIPE, SIG_IGN);
        }

        void signal_exit(int code) {
            ::exit(code);
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

        void poll_unblock_on(Poll& poll, const Notifier& notifier) {
            assert_true(static_cast<bool>(poll), "invalid notifier, cannot poll");
            poll_unblock_on(poll, notifier.read);
        }
        void poll_unblock_on(Poll& poll, const Handle& file_or_socket) {
            assert_true(!is_zero_handle(file_or_socket), "invalid notifier, cannot poll");
            struct ::epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = handle_to_fd(file_or_socket);
            int res = ::epoll_ctl(handle_to_fd(poll.handle), EPOLL_CTL_ADD, handle_to_fd(file_or_socket), &ev);
            assert_true(res != -1, "epoll_ctl call failed while adding file to epoll");
        }

        void block_until_poll_unblocks(Poll& poll) {
            assert_true(static_cast<bool>(poll), "invalid poll, cannot wait");
            struct ::epoll_event events[1];
            ::epoll_wait(handle_to_fd(poll.handle), events, 1, -1);
        }

    #else
        #error "Signal library not implemented for OS"
    #endif
}