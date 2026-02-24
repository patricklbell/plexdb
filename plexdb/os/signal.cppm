export module plexdb.os.signal;

import plexdb.base;

export namespace plexdb::os {
    struct SignalPipe {
        int read_fd  = -1;
        int write_fd = -1;
    };

    SignalPipe signal_pipe_create();
    void signal_pipe_destroy(SignalPipe& pipe);
    void signal_pipe_notify(SignalPipe& pipe);

    using SignalHandler = void(*)(int);
    void signal_register(int signal_number, SignalHandler handler);
}
