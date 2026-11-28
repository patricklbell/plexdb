module;
#include <stacktrace>

module plexdb.debug;

import plexdb.os.core;

namespace plexdb::debug {
    using ULL = unsigned long long;

    void write_stack_stderr(U64 skip_frames) {
        auto trace = std::stacktrace::current(skip_frames + 1);
        for (U64 i = 0; i < trace.size(); i++) {
            const std::string desc = trace[i].description();
            const std::string file = trace[i].source_file();
            write_stderr("  #%llu %s at %s:%u\n", ULL(i), desc.empty() ? "??" : desc.c_str(), file.empty() ? "??" : file.c_str(), trace[i].source_line());
        }
    }

    void write_async_stack_stderr() {
        if constexpr (!k_debug) {
            return;
        }
        write_stderr("async stack:\n");
        U64 depth = 0;
        for (const AsyncFrame* f = g_async_frame; f; f = f->parent, depth++) {
            const char* function = f->function ? f->function : "<unnamed coroutine>";
            if (f->file) {
                write_stderr("  #%llu %s (%s:%llu)\n", ULL(depth), function, f->file, ULL(f->line));
            } else {
                write_stderr("  #%llu %s\n", ULL(depth), function);
            }
        }
        if (depth == 0) {
            write_stderr("  <empty>\n");
        }
    }

    AutoString8 get_caller_name(Optional<String8> exclude) {
        for (const auto& entry : std::stacktrace::current(1)) {
            const std::string desc = entry.description();
            const String8     name{desc.c_str(), desc.size()};
            if (name.length > 0 && !(exclude && contains(name, *exclude))) {
                return AutoString8{name};
            }
        }
        return {};
    }

}

// GDB:  call plexdb_async_stack()
// LLDB: expr plexdb_async_stack()
extern "C" [[gnu::used]] void plexdb_async_stack() {
    plexdb::debug::write_async_stack_stderr();
}
