export module plexdb.debug;

import plexdb.base;

export namespace plexdb::debug {
    // printf-style write to stderr; unbuffered and allocation-free so crash handlers can use it.
    [[gnu::format(printf, 1, 2)]]
    void write_stderr(const char* fmt, ...);

    // Symbolized stack trace of the calling thread, skipping the innermost skip_frames frames.
    void write_stacktrace(U64 skip_frames = 0);

    // Chain of coroutines running on the calling thread. No-op unless PLEXDB_DEBUG.
    void write_async_stack();

    // Name of the innermost calling function whose name does not contain exclude; empty if none.
    AutoString8 caller_name(String8 exclude);

    // Failed asserts and fatal signals (SIGSEGV/SIGBUS/SIGILL/SIGFPE) write a report with both
    // stacks to stderr, then terminate. Asserts trap in debug builds and exit(1) otherwise; signals
    // re-raise with the default action so the exit status still reports the signal.
    void install_crash_report();
}
