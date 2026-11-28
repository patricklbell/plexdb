module;
#include <plexdb/macros/macros.h>

#include <stdarg.h>
#include <stdio.h>
#include <stacktrace>

#if PLEXDB_OS_LINUX
#include <signal.h>
#include <string.h>
#include <unistd.h>
#endif

module plexdb.debug;

import plexdb.coroutine.debug;
import plexdb.os;

namespace plexdb::debug {
    using ULL = unsigned long long;

    void write_stacktrace(U64 skip_frames) {
        auto trace = std::stacktrace::current(skip_frames + 1);
        for (U64 i = 0; i < trace.size(); i++) {
            const std::string desc = trace[i].description();
            const std::string file = trace[i].source_file();
            write_stderr("  #%llu %s at %s:%u\n", ULL(i), desc.empty() ? "??" : desc.c_str(), file.empty() ? "??" : file.c_str(), trace[i].source_line());
        }
    }

    AutoString8 caller_name(String8 exclude) {
        for (const auto& entry : std::stacktrace::current(1)) {
            const std::string desc = entry.description();
            const String8     name{desc.c_str(), desc.size()};
            if (name.length > 0 && !contains(name, exclude)) {
                return AutoString8{name};
            }
        }
        return {};
    }

    void write_async_stack() {
        if constexpr (!k_debug) {
            return;
        }
        write_stderr("async stack:\n");
        U64 depth = 0;
        for (const coroutine::debug::Frame* f = coroutine::debug::g_current_frame; f; f = f->parent, depth++) {
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

#if PLEXDB_OS_LINUX
    void write_stderr(const char* fmt, ...) {
        char    buf[1024];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n > 0) {
            ::write(2, buf, min(static_cast<U64>(n), sizeof(buf) - 1));
        }
    }

    static constexpr int k_fatal_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};

    // @note Symbolizing allocates, which is not async-signal-safe: a fault inside the allocator
    // can deadlock here. The alarm bounds that to a SIGALRM exit, and SA_RESETHAND makes a fault
    // inside the handler itself take the default action.
    static void fatal_signal_handler(int signal_number, siginfo_t* info, void*) {
        ::alarm(5);
        write_stderr("Fatal signal %d (%s) at address %p\n", signal_number, strsignal(signal_number), info->si_addr);
        write_stacktrace(1);
        write_async_stack();
        ::signal(signal_number, SIG_DFL);
        ::raise(signal_number);
    }

    static void crash_assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
        write_stderr("Assert failed \"%s\" at %s in %s:%u\n", msg, function_name, file_name, line_number);
        write_stacktrace(1);
        write_async_stack();
        if constexpr (k_debug) {
            // @note the trap must reach the debugger or core dump, not the fatal signal report
            for (int signal_number : k_fatal_signals) {
                ::signal(signal_number, SIG_DFL);
            }
            PLEXDB_TRAP;
        } else {
            os::process_exit(1);
        }
    }

    void install_crash_report() {
        set_assert_handler(crash_assert_handler);

        // Alternate stack so a stack overflow can still be reported.
        static U8 alt_stack[64 * 1024];
        stack_t   ss{.ss_sp = alt_stack, .ss_flags = 0, .ss_size = sizeof(alt_stack)};
        assert_true_always(::sigaltstack(&ss, nullptr) == 0, "sigaltstack failed");

        for (int signal_number : k_fatal_signals) {
            struct sigaction sa{};
            sa.sa_sigaction = fatal_signal_handler;
            ::sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
            assert_true_always(::sigaction(signal_number, &sa, nullptr) == 0, "sigaction failed");
        }
    }
#else
#error "Debug library not implemented for OS"
#endif
}

// GDB:  call plexdb_async_stack()
// LLDB: expr plexdb_async_stack()
extern "C" [[gnu::used]] void plexdb_async_stack() {
    plexdb::debug::write_async_stack();
}
