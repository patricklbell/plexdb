module;
#include "macros.h"

#if PLEXDB_OS_LINUX
    #include <unistd.h>
    #include <signal.h>
    #include <string.h>
#endif

module plexdb.os.signal;

namespace plexdb::os {
    #if PLEXDB_OS_LINUX

    SignalPipe signal_pipe_create() {
        int fds[2];
        int res = pipe(fds);
        assert_true_always(res == 0, "failed to create signal pipe");
        return { fds[0], fds[1] };
    }

    void signal_pipe_destroy(SignalPipe& pipe) {
        if (pipe.read_fd >= 0) close(pipe.read_fd);
        if (pipe.write_fd >= 0) close(pipe.write_fd);
        pipe.read_fd  = -1;
        pipe.write_fd = -1;
    }

    void signal_pipe_notify(SignalPipe& pipe) {
        char c = 0;
        [[maybe_unused]] auto res = write(pipe.write_fd, &c, 1);
    }

    void signal_register(int signal_number, SignalHandler handler) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        int res = sigaction(signal_number, &sa, nullptr);
        assert_true_always(res == 0, "sigaction failed");
    }

    #else
        #error "Signal library not implemented for OS"
    #endif
}
