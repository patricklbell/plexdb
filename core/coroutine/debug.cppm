module;
#include <source_location>

export module plexdb.coroutine.debug;

import plexdb.base;
import plexdb.debug;

export namespace plexdb::coroutine::debug {
    // Per-coroutine metadata stored inside each promise_type.
    using Frame = plexdb::debug::AsyncFrame;
    using plexdb::debug::g_async_frame;

    // wrap Frame* so [[no_unique_address]] can eliminate it.
    struct EmptyFrame {};
    struct FrameLink {
        Frame* ptr = nullptr;
    };

    inline void set_frame_info(std::source_location loc = std::source_location::current()) {
        if constexpr (!k_debug) {
            return;
        }
        if (!g_async_frame) {
            return;
        }
        g_async_frame->function = loc.function_name();
        g_async_frame->file     = loc.file_name();
        g_async_frame->line     = static_cast<U64>(loc.line());
    }

    // Sets frame to the first caller outside plexdb::coroutine.
    inline void capture_frame_from_stacktrace(Frame& frame) {
        frame.function_owned = plexdb::debug::get_caller_name(String8{"plexdb::coroutine"});
        frame.function       = frame.function_owned.c_str;
    }
    inline void capture_frame_from_stacktrace(EmptyFrame&) {
    }

    // Template helpers that make debug-frame accesses type-dependent so they
    // are correctly pruned in the discarded branch of if constexpr in base.cppm.
    template<typename F>
    inline void push_frame(F& frame) {
        if constexpr (!SameAs<F, EmptyFrame>) {
            frame.parent  = g_async_frame;
            g_async_frame = &frame;
        }
    }

    template<typename F>
    inline void pop_frame(const F& frame) {
        if constexpr (!SameAs<F, EmptyFrame>) {
            g_async_frame = frame.parent;
        }
    }

    template<typename L>
    inline void save_frame(L& link) {
        if constexpr (!SameAs<L, EmptyFrame>) {
            link.ptr = g_async_frame;
        }
    }

    template<typename L>
    inline void restore_frame(const L& link) {
        if constexpr (!SameAs<L, EmptyFrame>) {
            g_async_frame = link.ptr;
        }
    }
}
