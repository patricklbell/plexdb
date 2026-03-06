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

    void poll_unblock_on(Poll& poll, const Notifier& notifier);
    void poll_unblock_on(Poll& poll, const Handle& file_or_socket);

    // @note blocking call
    void block_until_poll_unblocks(Poll& poll);
}