module;
#include <source_location>

export module plexdb.coroutine.debug;

import plexdb.base;
import plexdb.debug;

export namespace plexdb::coroutine::debug {
    // Per-coroutine metadata stored inside each promise_type.
    // Forms a singly-linked list from the innermost running frame to the root.
    struct Frame {
        const char* function = nullptr;
        AutoString8 function_owned;
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
        if constexpr (!k_debug) {
            return;
        }
        if (!g_current_frame) {
            return;
        }
        g_current_frame->function = loc.function_name();
        g_current_frame->file     = loc.file_name();
        g_current_frame->line     = static_cast<U64>(loc.line());
    }

    // Called from get_return_object(): names the frame after the first caller outside plexdb::coroutine.
    inline void capture_frame_from_stacktrace(Frame& frame) {
        frame.function_owned = plexdb::debug::caller_name("plexdb::coroutine");
        frame.function       = frame.function_owned.c_str;
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
}
