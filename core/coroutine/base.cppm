module;
#include <coroutine>
#include <exception>

#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>

export module plexdb.coroutine.base;

import plexdb.base;
import plexdb.coroutine.debug;
import plexdb.debug;

// ============================================================================
// Tracy fiber tracking
//
// Tracy attributes every ZoneBegin/ZoneEnd to whichever fiber (or the bare
// thread) is current on the emitting thread at that instant, and a ZoneEnd
// carries no zone id: the server just pops the current context's zone stack.
// A ScopedZone in a coroutine body stays open across suspensions, so its
// destructor must run with the *same* context current as its constructor, and
// that context's zones must be strictly nested. Otherwise the server stops
// the capture with:
//   "Zone is ended twice."                         (popped an empty stack)
//   "Invalid order of zone begin and end events."  (popped somebody else's zone)
//
// That gives two rules:
//
//   1. One fiber per *strand*. Coroutines whose zones can interleave in time
//      need distinct fibers. A child Task that is co_awaited shares its
//      parent's fiber (the parent is parked until the child finishes, so their
//      zones nest). Roots (Task::resume(), Start::Eager) and every when_all
//      Runner get a fiber of their own, named after their frame address.
//
//   2. The current context is restored on every way out of a coroutine body.
//      Resumption is an ordinary nested call, and the resumer is not
//      necessarily the event loop: any coroutine may resume another inline,
//      and an eager coroutine is entered directly from its creator's body.
//      The resumer expects its own context back when that call returns. So:
//        - entering a body after a fresh resume pushes the resumer's context
//          and switches to the body's fiber        (fiber::enter)
//        - suspending back to the resumer pops it   (fiber::suspend_to_resumer)
//        - symmetric transfer (handle-returning await_suspend) hands the same
//          resumer level to the target without push/pop
//                                                   (fiber::suspend_by_transfer)
//        - destroying a suspended frame runs its zone destructors, so it is
//          done inside the frame's fiber             (fiber::destroy_frame)
//
// @note every awaiter must call these at each entry to and exit from a body;
// an awaiter that skips them breaks rule 2 for every coroutine above it.
// @profile inline in the interface: runs on every coroutine switch.
// ============================================================================
export namespace plexdb::coroutine::fiber {
    inline constexpr bool enabled =
#ifdef PLEXDB_ENABLE_TRACY_PROFILER
        true;
#else
        false;
#endif

    // Tracy keys fibers by name *pointer*, so a name must stay valid while its
    // fiber is in use: names live in the owning coroutine frame.
    struct FrameState {
        // "0x" + hex digits for pointer + NUL
        static constexpr int k_len      = 2 + static_cast<int>(sizeof(void*)) * 2 + 1;
        char                 own[k_len] = {};
        // fiber this frame's body executes in: `own` for roots/runners/eager
        // tasks, the awaiting parent's fiber for co_awaited lazy tasks
        const char* runs_in = nullptr;
        // fiber of the coroutine co_awaiting this one (Task only)
        const char* return_to = nullptr;
    };
    struct EmptyFrameState {};
    using FrameStateT = Conditional<enabled, FrameState, EmptyFrameState>;

    struct Saved {
        const char* fiber = nullptr;
    };
    struct EmptySaved {};
    using SavedT = Conditional<enabled, Saved, EmptySaved>;

    // Context Tracy currently considers active on this thread (nullptr = the
    // thread itself). Must only be changed through switch_to().
    inline thread_local const char* g_current = nullptr;

    // Context to restore whenever the innermost h.resume() returns to its caller.
    inline constexpr U32            k_max_resume_depth = 256;
    inline thread_local const char* g_resumer_stack[k_max_resume_depth]{};
    inline thread_local U32         g_resumer_depth = 0;

    // Set by a symmetric-transfer await_suspend and consumed by the target's
    // await_resume, which runs immediately afterwards on the same thread.
    inline thread_local bool g_transfer_pending = false;

    inline void switch_to(const char* f) noexcept {
        if constexpr (enabled) {
            if (f == g_current) {
                return;
            }
            g_current = f;
            if (f) {
                TracyFiberEnter(f);
            } else {
                TracyFiberLeave;
            }
        }
    }

    // A coroutine body is about to run (again) in fiber `f`.
    inline void enter(const char* f) noexcept {
        if constexpr (enabled) {
            if (g_transfer_pending) {
                g_transfer_pending = false;
            } else {
                assert_true(g_resumer_depth < k_max_resume_depth, "tracy fiber: nested coroutine resumption too deep");
                g_resumer_stack[g_resumer_depth++] = g_current;
            }
            switch_to(f);
        }
    }

    // A coroutine body suspends and control returns to whoever resumed it.
    inline void suspend_to_resumer() noexcept {
        if constexpr (enabled) {
            assert_true(g_resumer_depth > 0, "tracy fiber: coroutine suspended without a matching enter()");
            switch_to(g_resumer_stack[--g_resumer_depth]);
        }
    }

    // A coroutine body suspends by returning another coroutine's handle from
    // await_suspend. The target must be resumed in one of this module's
    // awaiters, whose await_resume calls enter().
    inline void suspend_by_transfer() noexcept {
        if constexpr (enabled) {
            g_transfer_pending = true;
        }
    }

    // Re-entry for an awaiter that may not have suspended at all (await_ready
    // returned true): only a pending transfer means the body actually left.
    inline void enter_if_transferred(const char* f) noexcept {
        if constexpr (enabled) {
            if (g_transfer_pending) {
                enter(f);
            }
        }
    }

    inline void name_frame(FrameState& s, const void* frame) noexcept {
        constexpr char k_hex[] = "0123456789abcdef";
        auto           v       = reinterpret_cast<uintptr_t>(frame);
        constexpr int  n       = static_cast<int>(sizeof(void*)) * 2;
        s.own[0]               = '0';
        s.own[1]               = 'x';
        for (int i = n - 1; i >= 0; --i) {
            s.own[2 + i] = k_hex[v & 0xf];
            v >>= 4;
        }
        s.own[2 + n] = '\0';
    }
    inline void name_frame(EmptyFrameState&, const void*) noexcept {
    }

    // Root of a new strand: run in the frame's own fiber.
    inline void run_in_own(FrameState& s) noexcept {
        s.runs_in = s.own;
    }
    inline void run_in_own(EmptyFrameState&) noexcept {
    }

    // About to be co_awaited from the current context. `share` = run in the
    // awaiter's fiber (lazy child) rather than keep an own fiber (eager task).
    inline void awaited_from_current(FrameState& s, bool share) noexcept {
        s.return_to = g_current;
        if (share) {
            s.runs_in = g_current;
        }
    }
    inline void awaited_from_current(EmptyFrameState&, bool) noexcept {
    }

    inline const char* runs_in(const FrameState& s) noexcept {
        return s.runs_in;
    }
    inline const char* runs_in(const EmptyFrameState&) noexcept {
        return nullptr;
    }
    inline const char* return_to(const FrameState& s) noexcept {
        return s.return_to;
    }
    inline const char* return_to(const EmptyFrameState&) noexcept {
        return nullptr;
    }

    inline void save_current(Saved& s) noexcept {
        s.fiber = g_current;
    }
    inline void save_current(EmptySaved&) noexcept {
    }
    inline const char* saved(const Saved& s) noexcept {
        return s.fiber;
    }
    inline const char* saved(const EmptySaved&) noexcept {
        return nullptr;
    }

    // Destroying a frame that is still suspended runs the destructors of its
    // live locals, including open ScopedZones (and, recursively, those of any
    // child frames it owns). Those must end in the fiber they began in.
    template<typename Promise>
    void destroy_frame(std::coroutine_handle<Promise> h) noexcept {
        if constexpr (enabled) {
            // runs_in is null only for a lazy task that never started (no zones)
            const char* f = runs_in(h.promise().fiber_state);
            if (!h.done() && f) {
                const char* prev = g_current;
                switch_to(f);
                h.destroy();
                switch_to(prev);
                return;
            }
        }
        h.destroy();
    }
}

namespace plexdb::coroutine {
    struct EmptyManualResumeFlag {
        EmptyManualResumeFlag& operator=(bool) noexcept {
            return *this;
        }
        explicit operator bool() const noexcept {
            return false;
        }
    };
}

export namespace plexdb::coroutine {
    // ========================================================================
    // Start
    //   Controls whether a Task suspends before its body executes (Lazy),
    //   or begins executing inline during construction (Eager).
    // ========================================================================
    enum class Start {
        Lazy,
        Eager
    };

    // ========================================================================
    // Task<T, S>
    //   @note S=Start::Lazy (default): coroutine body does not run until
    //         resume() or co_await. Safe to create before continuations exist.
    //         S=Start::Eager: body runs inline during construction up to the
    //         first real suspension or co_return.
    // ========================================================================
    template<typename T = void, Start S = Start::Lazy>
    struct [[nodiscard]] Task;

    template<typename T, Start S>
    struct [[nodiscard]] Task {
        // ====================================================================
        // built-in api (e.g. co_await)
        // ====================================================================
        struct promise_type {
            Optional<T> result;
            // null until co_awaited: nobody to transfer back to on completion
            std::coroutine_handle<> continuation{};
#ifdef PLEXDB_EXCEPTIONS
            std::exception_ptr exception;
#endif
            // @note zero bytes in release builds via [[no_unique_address]] + empty type.
            [[no_unique_address]]
            Conditional<k_debug, debug::Frame, debug::EmptyFrame> debug_frame;

            [[no_unique_address]]
            fiber::FrameStateT fiber_state;

            [[no_unique_address]]
            Conditional<k_debug, bool, EmptyManualResumeFlag> _manually_resumed{};

            Task get_return_object() noexcept {
                debug::capture_frame_from_stacktrace(debug_frame);
                auto h = std::coroutine_handle<promise_type>::from_promise(*this);
                fiber::name_frame(fiber_state, h.address());
                // Eager tasks run concurrently with their creator from the
                // first suspension on, so they are their own strand.
                if constexpr (S == Start::Eager) {
                    fiber::run_in_own(fiber_state);
                }
                return Task{h};
            }

            auto initial_suspend() noexcept {
                struct Awaiter {
                    promise_type& p;
                    bool          await_ready() const noexcept {
                        return S == Start::Eager;
                    }
                    void await_suspend(std::coroutine_handle<>) const noexcept {
                    }
                    // first entry into the body: fresh for Eager/resume(),
                    // a transfer when started by co_await
                    void await_resume() const noexcept {
                        fiber::enter(fiber::runs_in(p.fiber_state));
                    }
                };
                return Awaiter{*this};
            }

            auto final_suspend() noexcept {
                struct Awaiter {
                    bool await_ready() noexcept {
                        return false;
                    }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        debug::pop_frame(h.promise().debug_frame);

                        if (h.promise().continuation) {
                            fiber::suspend_by_transfer();
                            return h.promise().continuation;
                        }
                        // not awaited (a root): control returns to the resumer
                        fiber::suspend_to_resumer();
                        return std::noop_coroutine();
                    }
                    void await_resume() noexcept {
                    }
                };
                return Awaiter{};
            }

            void return_value(T value) {
                result.emplace(move(value));
            }
            void unhandled_exception() {
#ifdef PLEXDB_EXCEPTIONS
                exception = std::current_exception();
#endif
                if constexpr (k_debug) {
                    plexdb::debug::write_stderr("unhandled exception in coroutine\n");
                    plexdb::debug::write_async_stack_stderr();
                }
            }
        };

        bool await_ready() const noexcept {
            return handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            if constexpr (k_debug) {
                assert_true(!handle.promise()._manually_resumed, "Task::await_suspend: task was already manually resumed (double-resume)");
            }

            debug::push_frame(handle.promise().debug_frame);

            fiber::awaited_from_current(handle.promise().fiber_state, S == Start::Lazy);

            handle.promise().continuation = caller;
            fiber::suspend_by_transfer();
            return handle;
        }

        T await_resume() {
            // parent resumes via the child's final_suspend transfer
            fiber::enter_if_transferred(fiber::return_to(handle.promise().fiber_state));
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception) {
                std::rethrow_exception(handle.promise().exception);
            }
#endif
            return move(handle.promise().result.value());
        }

        // ====================================================================
        // manual api
        // ====================================================================
        void resume() {
            if constexpr (k_debug) {
                handle.promise()._manually_resumed = true;
            }
            // a manually resumed task is a root: its own strand
            fiber::run_in_own(handle.promise().fiber_state);
            // @todo manual resume() is an async-stack root: await_suspend/
            // final_suspend push/pop debug_frame in matched pairs for
            // co_await chains, but nothing pairs with a direct .resume() call
            Conditional<k_debug, debug::FrameLink, debug::EmptyFrame> _saved_frame{};
            debug::save_frame(_saved_frame);
            if constexpr (k_debug) {
                debug::g_async_frame = nullptr;
            }
            handle.resume();
            debug::restore_frame(_saved_frame);
        }
        bool done() const {
            return handle.done();
        }
        bool has_value() const {
            return handle.promise().result.has_value();
        }
        T& value() & {
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception) {
                std::rethrow_exception(handle.promise().exception);
            }
#endif
            assert_true(has_value(), "coroutine result is not ready");
            return handle.promise().result.value();
        }
        const T& value() const& {
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception) {
                std::rethrow_exception(handle.promise().exception);
            }
#endif
            assert_true(has_value(), "coroutine result is not ready");
            return handle.promise().result.value();
        }

        // ====================================================================
        // constructors/destructors
        // ====================================================================
        Task(Task&& other) noexcept
            : handle(other.handle) {
            other.handle = nullptr;
        }

        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                destroy();
                handle       = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        Task(const Task&)            = delete;
        Task& operator=(const Task&) = delete;
        ~Task() {
            destroy();
        }

    private:
        std::coroutine_handle<promise_type> handle;
        explicit Task(std::coroutine_handle<promise_type> h) noexcept
            : handle(h) {
        }
        void destroy() {
            if (handle) {
                fiber::destroy_frame(handle);
                handle = nullptr;
            }
        }
    };

    template<Start S>
    struct [[nodiscard]] Task<void, S> {
        // ====================================================================
        // built-in api (e.g. co_await)
        // ====================================================================
        struct promise_type {
            // null until co_awaited: nobody to transfer back to on completion
            std::coroutine_handle<> continuation{};
#ifdef PLEXDB_EXCEPTIONS
            std::exception_ptr exception;
#endif
            [[no_unique_address]]
            Conditional<k_debug, debug::Frame, debug::EmptyFrame> debug_frame;

            [[no_unique_address]]
            fiber::FrameStateT fiber_state;

            [[no_unique_address]]
            Conditional<k_debug, bool, EmptyManualResumeFlag> _manually_resumed{};

            Task get_return_object() noexcept {
                debug::capture_frame_from_stacktrace(debug_frame);
                auto h = std::coroutine_handle<promise_type>::from_promise(*this);
                fiber::name_frame(fiber_state, h.address());
                // Eager tasks run concurrently with their creator from the
                // first suspension on, so they are their own strand.
                if constexpr (S == Start::Eager) {
                    fiber::run_in_own(fiber_state);
                }
                return Task{h};
            }

            auto initial_suspend() noexcept {
                struct Awaiter {
                    promise_type& p;
                    bool          await_ready() const noexcept {
                        return S == Start::Eager;
                    }
                    void await_suspend(std::coroutine_handle<>) const noexcept {
                    }
                    // first entry into the body: fresh for Eager/resume(),
                    // a transfer when started by co_await
                    void await_resume() const noexcept {
                        fiber::enter(fiber::runs_in(p.fiber_state));
                    }
                };
                return Awaiter{*this};
            }

            auto final_suspend() noexcept {
                struct Awaiter {
                    // @note coroutine pauses after final_suspend, meaning ~Task needs to call destroy
                    bool await_ready() noexcept {
                        return false;
                    }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        debug::pop_frame(h.promise().debug_frame);

                        if (h.promise().continuation) {
                            fiber::suspend_by_transfer();
                            return h.promise().continuation;
                        }
                        // not awaited (a root): control returns to the resumer
                        fiber::suspend_to_resumer();
                        return std::noop_coroutine();
                    }
                    void await_resume() noexcept {
                    }
                };
                return Awaiter{};
            }

            void return_void() noexcept {
            }
            void unhandled_exception() {
#ifdef PLEXDB_EXCEPTIONS
                exception = std::current_exception();
#endif
                if constexpr (k_debug) {
                    plexdb::debug::write_stderr("unhandled exception in coroutine\n");
                    plexdb::debug::write_async_stack_stderr();
                }
            }
        };

        bool await_ready() const noexcept {
            return handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            if constexpr (k_debug) {
                assert_true(!handle.promise()._manually_resumed, "Task::await_suspend: task was already manually resumed (double-resume)");
            }

            debug::push_frame(handle.promise().debug_frame);

            fiber::awaited_from_current(handle.promise().fiber_state, S == Start::Lazy);

            handle.promise().continuation = caller;
            fiber::suspend_by_transfer();
            return handle;
        }

        void await_resume() {
            // parent resumes via the child's final_suspend transfer
            fiber::enter_if_transferred(fiber::return_to(handle.promise().fiber_state));
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception) {
                std::rethrow_exception(handle.promise().exception);
            }
#endif
        }

        // ====================================================================
        // manual api
        // ====================================================================
        void resume() {
            if constexpr (k_debug) {
                handle.promise()._manually_resumed = true;
            }
            // a manually resumed task is a root: its own strand
            fiber::run_in_own(handle.promise().fiber_state);

            Conditional<k_debug, debug::FrameLink, debug::EmptyFrame> _saved_frame{};
            debug::save_frame(_saved_frame);
            if constexpr (k_debug) {
                debug::g_async_frame = nullptr;
            }
            handle.resume();
            debug::restore_frame(_saved_frame);
        }
        bool done() const {
            return handle.done();
        }

        // ====================================================================
        // constructors/destructors
        // ====================================================================
        Task(Task&& other) noexcept
            : handle(other.handle) {
            other.handle = nullptr;
        }

        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                destroy();
                handle       = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        Task(const Task&)            = delete;
        Task& operator=(const Task&) = delete;
        ~Task() {
            destroy();
        }

    private:
        std::coroutine_handle<promise_type> handle;
        explicit Task(std::coroutine_handle<promise_type> h) noexcept
            : handle(h) {
        }
        void destroy() {
            if (handle) {
                fiber::destroy_frame(handle);
                handle = nullptr;
            }
        }
    };

    // ========================================================================
    //   Generic bridge between a coroutine and an external async event source
    //   (e.g. io_uring, timers, inter-thread signals).
    //
    //   OnSuspend: void(std::coroutine_handle<>)
    //     Called when the coroutine suspends. Register the handle and submit
    //     the async operation.
    //
    //   OnResume: Result()
    //     Called to extract the result after the coroutine is woken.
    //
    //   OnDelete: void() — must be TriviallyCopyable (no reference captures)
    //     Called in ~Awaitable only if the coroutine was destroyed while
    //     suspended (i.e. await_resume was never called). Use to undo
    //     whatever on_suspend registered. Capture pointers, not references —
    //     the TriviallyCopyable constraint enforces this at compile time.
    // ========================================================================
    struct NoOp {
        void operator()() const noexcept {
        }
    };

    template<typename OnSuspend, typename OnResume, typename OnDelete = NoOp>
        requires TriviallyCopyable<OnDelete>
    struct [[nodiscard]] Awaitable {
        OnSuspend on_suspend;
        OnResume  on_resume;
        OnDelete  on_delete;
        bool      _resumed = false;

        Awaitable(OnSuspend s, OnResume r)
            : on_suspend(move(s))
            , on_resume(move(r)) {
        }
        Awaitable(OnSuspend s, OnResume r, OnDelete d)
            : on_suspend(move(s))
            , on_resume(move(r))
            , on_delete(move(d)) {
        }

        bool await_ready() noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            debug::save_frame(_saved_frame);

            // Hand the thread back to the resumer's context *before* on_suspend:
            // the resumer is not necessarily the event loop (another coroutine
            // may have resumed us inline), and on_suspend may itself resume h.
            fiber::save_current(_saved_fiber);
            fiber::suspend_to_resumer();

            on_suspend(h);
        }

        decltype(auto) await_resume() noexcept {
            _resumed = true;

            debug::restore_frame(_saved_frame);

            // fresh resume (event loop / another coroutine) or a transfer (Latch)
            fiber::enter(fiber::saved(_saved_fiber));

            return on_resume();
        }

        ~Awaitable() {
            if (!_resumed) {
                on_delete();
            }
        }

        [[no_unique_address]]
        Conditional<k_debug, debug::FrameLink, debug::EmptyFrame> _saved_frame{};

        [[no_unique_address]]
        fiber::SavedT _saved_fiber{};
    };

    template<typename OnSuspend, typename OnResume>
    Awaitable(OnSuspend, OnResume) -> Awaitable<OnSuspend, OnResume, NoOp>;

    template<typename OnSuspend, typename OnResume, typename OnDelete>
    Awaitable(OnSuspend, OnResume, OnDelete) -> Awaitable<OnSuspend, OnResume, OnDelete>;

    // ========================================================================
    // generator
    //   @note lazy
    // ========================================================================
    template<typename T>
    struct [[nodiscard]] Generator {
        // ====================================================================
        // built-in api (e.g. co_await)
        // ====================================================================
        struct promise_type {
            Optional<T> current;

            Generator get_return_object() noexcept {
                return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept {
                return {};
            }
            std::suspend_always final_suspend() noexcept {
                return {};
            }

            std::suspend_always yield_value(T value) {
                current.emplace(move(value));
                return {};
            }

            void return_void() noexcept {
            }
            void unhandled_exception() {
            }
        };

        struct Iterator {
            std::coroutine_handle<promise_type> handle;

            Iterator& operator++() {
                assert_true(handle && !handle.done(), "incremented past-end generator iterator");
                handle.resume();
                if (handle.done()) {
                    handle = nullptr;
                }
                return *this;
            }

            T& operator*() const {
                return handle.promise().current.value();
            }
            T* operator->() const {
                return &handle.promise().current.value();
            }

            bool operator==(const Iterator& other) const noexcept {
                return handle == other.handle;
            }

            bool operator!=(const Iterator& other) const noexcept {
                return handle != other.handle;
            }
        };

        Iterator begin() {
            if (handle) {
                handle.resume();
            }
            if (!handle || handle.done()) {
                return end();
            }
            return Iterator{handle};
        }

        Iterator end() noexcept {
            return Iterator{nullptr};
        }

        // ====================================================================
        // manual api
        // ====================================================================
        Optional<T> next() {
            if (!handle || handle.done()) {
                return {};
            }
            handle.resume();
            if (handle.done()) {
                return {};
            }
            return Optional<T>{move(handle.promise().current.value())};
        }

        // ====================================================================
        // constructors/destructors
        // ====================================================================
        Generator(Generator&& other) noexcept
            : handle(other.handle) {
            other.handle = nullptr;
        }

        Generator& operator=(Generator&& other) noexcept {
            if (this != &other) {
                destroy();
                handle       = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        Generator(const Generator&)            = delete;
        Generator& operator=(const Generator&) = delete;
        ~Generator() {
            destroy();
        }

    private:
        std::coroutine_handle<promise_type> handle;
        explicit Generator(std::coroutine_handle<promise_type> h) noexcept
            : handle(h) {
        }
        void destroy() {
            if (handle) {
                handle.destroy();
                handle = nullptr;
            }
        }
    };
}
