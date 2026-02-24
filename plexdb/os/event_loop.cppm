module;
#include "macros.h"

#if PLEXDB_OS_LINUX
    #include <sys/epoll.h>
    #if PLEXDB_HAS_IO_URING
        #include <atomic>
        #include <liburing.h>
    #endif
#endif

export module plexdb.os.event_loop;

import plexdb.base;

using namespace plexdb;

export namespace plexdb::os {
    constexpr int EVENT_LOOP_MAX_DRAIN = 256;

    struct CompletionEvent {
        int result;
        U64 user_data;
    };

    struct IoBuffer {
        void* base;
        U64 length;
    };

    struct EventLoop {
        enum class Backend : U8 { IoUring, Epoll };
        Backend backend;
        int signal_fd;
        int epoll_fd;

        #if PLEXDB_HAS_IO_URING
            io_uring ring;
        #endif
    };

    EventLoop event_loop_create(U32 queue_depth, int signal_fd);
    void event_loop_destroy(EventLoop& loop);

    bool event_loop_register_buffers(EventLoop& loop, IoBuffer* buffers, int count);
    void event_loop_unregister_buffers(EventLoop& loop);

    bool event_loop_submit_accept(EventLoop& loop, int listen_fd, U64 user_data);
    bool event_loop_submit_multishot_accept(EventLoop& loop, int listen_fd, U64 user_data);
    bool event_loop_submit_read(EventLoop& loop, int fd, void* buf, U64 len, int buf_index, U64 user_data);
    bool event_loop_submit_write(EventLoop& loop, int fd, const void* buf, U64 len, int buf_index, U64 user_data);
    bool event_loop_submit_close(EventLoop& loop, int fd, U64 user_data);

    int event_loop_flush(EventLoop& loop);

    // @note returns completions and consumes them, up to max_events
    int event_loop_drain(EventLoop& loop, CompletionEvent* events, int max_events);

    // @note blocks until events are available, interruptible via signal_fd
    void event_loop_wait(EventLoop& loop);
}
