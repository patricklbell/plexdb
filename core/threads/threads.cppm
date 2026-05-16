module;
#include <plexdb/macros/macros.h>

export module plexdb.threads;

import plexdb.base;
import plexdb.arena;
import plexdb.os.core;
import plexdb.os.thread;
import plexdb.os.function;

export namespace plexdb::threads {
    struct Context {
        Arena arenas[2];
        bool is_main = false;
    };

    void     equip(Context* in_ctx);
    Context* get_context();
    Arena*   get_scratch(TArrayView<Arena*,U64> conflicts);

    // ========================================================================
    // thread RAII wrapper
    // ========================================================================
    struct Thread {
        os::Handle handle = os::zero_handle();

        Thread() = default;
        explicit Thread(os::Handle h) : handle(h) {}
        ~Thread();

        Thread(const Thread&) = delete;
        Thread& operator=(const Thread&) = delete;

        Thread(Thread&& other) noexcept;
        Thread& operator=(Thread&& other) noexcept;

        operator os::Handle() const { return handle; }
        operator bool() const;
    };

    Thread launch(AutoFunctor<void()> fn, const char* name = nullptr);

    template<typename F>
        requires (!SameAs<RemoveCVRef<F>, AutoFunctor<void()>>)
    Thread launch(F&& fn, const char* name = nullptr) {
        return launch(AutoFunctor<void()>(forward<F>(fn)), name);
    }

    template<typename F>
    Thread launch(const char* name, F&& fn) {
        return launch(AutoFunctor<void()>(forward<F>(fn)), name);
    }

    // ========================================================================
    // semaphore RAII wrapper
    // ========================================================================
    struct Semaphore {
        os::Handle handle = os::zero_handle();

        explicit Semaphore(U32 initial_count = 0);
        ~Semaphore();

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        Semaphore(Semaphore&& other) noexcept;
        Semaphore& operator=(Semaphore&& other) noexcept;

        void signal();
        void wait();
    };

    // ========================================================================
    // scratch arena
    // ========================================================================
    struct Scope {
        Arena* arena;
        U64 offset;
        bool active = true;

        Scope(Arena* in_arena);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        Scope(Scope&&) noexcept;
        Scope& operator=(Scope&&) noexcept;
    };

    inline Scope scratch(TArrayView<Arena*,U64> conflicts) { return Scope(get_scratch(conflicts)); }
    inline Scope scratch()                                 { return scratch(TArrayView<Arena*,U64>()); }
}
