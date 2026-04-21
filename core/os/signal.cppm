module;
#include "macros.h"

export module plexdb.os.signal;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb::os {
    struct Notifier {
        Handle read  = zero_handle();
        Handle write = zero_handle();

        Notifier();
        ~Notifier();

        Notifier(const Notifier&) = delete;
        Notifier& operator=(const Notifier&) = delete;

        Notifier(Notifier&& other) noexcept;
        Notifier& operator=(Notifier&& other) noexcept;

        explicit operator bool() const;
    };

    // @note safe if notifier is closed
    void signal_notify_safe(Notifier& notifier);

    // @todo wrap int
    using SignalHandler = void(*)(int);

    void signal_register_kill(SignalHandler handler);
    void signal_register_stop(SignalHandler handler);
    void signal_ignore_pipe();
    void signal_exit(int code);

    // Platform signal numbers for use with process_kill.
#if PLEXDB_OS_LINUX
    constexpr S32 SIGNAL_KILL = 9;   // SIGKILL
    constexpr S32 SIGNAL_USR1 = 10;  // SIGUSR1
    constexpr S32 SIGNAL_TERM = 15;  // SIGTERM
#endif

    struct Poll {
        Handle handle;

        Poll();
        ~Poll();

        Poll(const Poll&) = delete;
        Poll& operator=(const Poll&) = delete;

        Poll(Poll&& other) noexcept;
        Poll& operator=(Poll&& other) noexcept;

        explicit operator bool() const;
    };

    enum class PollEventMask : U32 {
        None  = 0,
        Read  = 1 << 0,
        Write = 1 << 1,
        Error = 1 << 2,
        HangUp = 1 << 3,
    };

    constexpr PollEventMask operator|(PollEventMask a, PollEventMask b) {
        return PollEventMask(U32(a) | U32(b));
    }

    struct PollEvent {
        Handle handle = zero_handle();
        PollEventMask events = PollEventMask::None;
    };

    void poll_unblock_on(Poll& poll, const Notifier& notifier);
    void poll_unblock_on(Poll& poll, const Handle& file_or_socket);
    void poll_unblock_on(Poll& poll, const Handle& file_or_socket, PollEventMask events);
    void poll_update(Poll& poll, const Handle& file_or_socket, PollEventMask events);

    // @note blocking call
    bool block_until_poll_unblocks(Poll& poll);
    bool poll_wait(Poll& poll, CappedTArrayView<PollEvent>* out_events);
}