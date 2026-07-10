module;
#include <source_location>
#if PLEXDB_DEBUG
#include <stacktrace>
#endif
#include <plexdb/macros/macros.h>

export module plexdb.coroutine.debug;

import plexdb.base;

export namespace plexdb::coroutine::debug {
    inline constexpr bool enabled = PLEXDB_DEBUG;

    // Per-coroutine metadata stored inside each promise_type.
    // Forms a singly-linked list from the innermost running frame to the root.
    struct Frame {
        const char* function = nullptr;
#if PLEXDB_DEBUG
        AutoString8 function_owned;
#endif
        const char* file   = nullptr;
        U64         line   = 0;
        Frame*      parent = nullptr;
    };

    // wrap Frame* so [[no_unique_address]] can eliminate it.
    struct EmptyFrame {};
    struct FrameLink {
        Frame* ptr = nullptr;
    };

    // points to the innermost (most recent) running coroutine's frame.
    // Null in non-coroutine context.
    inline thread_local Frame* g_current_frame = nullptr;

    inline void set_frame_info(std::source_location loc = std::source_location::current()) {
        if constexpr (!enabled) {
            return;
        }
        if (!g_current_frame) {
            return;
        }
        g_current_frame->function = loc.function_name();
        g_current_frame->file     = loc.file_name();
        g_current_frame->line     = static_cast<U64>(loc.line());
    }

    // Called from get_return_object() to auto-populate the frame name.
    // Walks std::stacktrace::current(), skips plexdb::coroutine infrastructure,
    // and stores the first user frame's description.
    inline void capture_frame_from_stacktrace([[maybe_unused]] Frame& frame) {
        if constexpr (!enabled) {
            return;
        }
#if PLEXDB_DEBUG
        for (const auto& entry : std::stacktrace::current()) {
            auto    desc_std = entry.description();
            String8 desc{desc_std.c_str(), desc_std.size()};
            if (desc.length > 0 && !contains(desc, "plexdb::coroutine")) {
                frame.function_owned = AutoString8{desc};
                frame.function       = frame.function_owned.c_str;
                break;
            }
        }
#endif
    }
    inline void capture_frame_from_stacktrace(EmptyFrame&) {
    }

    // Template helpers that make debug-frame accesses type-dependent so they
    // are correctly pruned in the discarded branch of if constexpr in base.cppm.
    template<typename F>
    inline void push_frame(F& frame) {
        if constexpr (!SameAs<F, EmptyFrame>) {
            frame.parent    = g_current_frame;
            g_current_frame = &frame;
        }
    }

    template<typename F>
    inline void pop_frame(const F& frame) {
        if constexpr (!SameAs<F, EmptyFrame>) {
            g_current_frame = frame.parent;
        }
    }

    template<typename L>
    inline void save_frame(L& link) {
        if constexpr (!SameAs<L, EmptyFrame>) {
            link.ptr = g_current_frame;
        }
    }

    template<typename L>
    inline void restore_frame(const L& link) {
        if constexpr (!SameAs<L, EmptyFrame>) {
            g_current_frame = link.ptr;
        }
    }

    inline void print_async_stack() {
        if constexpr (!enabled) {
            return;
        }
        const Frame* frame = g_current_frame;
        U64          depth = 0;
        while (frame) {
            // @note both file and function may not be provided
            if (frame->function && frame->file) {
                println("  #", to_str(depth), " ", frame->function, " (", frame->file, ":", to_str(frame->line), ")");
            } else if (frame->function) {
                println("  #", to_str(depth), " ", frame->function);
            } else {
                println("  #", to_str(depth), " <unnamed coroutine>");
            }
            frame = frame->parent;
            ++depth;
        }
        if (depth == 0) {
            println("  <empty async stack>");
        }
    }
}

// GDB:  call plexdb_async_stack()
// LLDB: expr plexdb_async_stack()
extern "C" void plexdb_async_stack() {
    const plexdb::coroutine::debug::Frame* f = plexdb::coroutine::debug::g_current_frame;
    int                                    d = 0;
    while (f) {
        if (f->function && f->file) {
            plexdb::println(plexdb::fmt("  #%d %s (%s:%llu)\n", d, f->function, f->file, static_cast<unsigned long long>(f->line)));
        } else if (f->function) {
            plexdb::println(plexdb::fmt("  #%d %s\n", d, f->function));
        } else {
            plexdb::println(plexdb::fmt("  #%d <unnamed coroutine>\n", d));
        }
        f = f->parent;
        ++d;
    }
    if (!d) {
        plexdb::println("  <empty async stack>\n");
    }
}
