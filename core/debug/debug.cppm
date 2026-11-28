export module plexdb.debug;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb::debug {
    // call stack of the coroutines running on a thread maintained by coroutines
    struct AsyncFrame {
        const char* function = nullptr;
        AutoString8 function_owned;
        const char* file   = nullptr;
        U64         line   = 0;
        AsyncFrame* parent = nullptr;
    };
    inline thread_local AsyncFrame* g_async_frame = nullptr;

    // @note intentionally unbuffered so crash handlers can use it.
    template<typename... Args>
    void write_stderr(const char* fmt, Args... args) {
        char buf[512];
        auto flush = [](TArrayView<char>& b, U64 length) {
            os::stream_write(os::stderr_stream(), b.ptr, length);
        };
        BufferedString8 out{
            TArrayView<char>{buf, sizeof(buf)},
            flush
        };
        append_fmt(out, fmt, args...);
    }

    // @note unbuffered and safe inside handlers, but symbolizing allocates, so a crash inside the
    // allocator can deadlock here.
    void write_stack_stderr(U64 skip_frames = 0);
    void write_async_stack_stderr();

    AutoString8 get_caller_name(Optional<String8> exclude = {});
}
