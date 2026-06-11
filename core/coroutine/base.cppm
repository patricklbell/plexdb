module;
#include <coroutine>
#include <exception>

#include <plexdb/macros/macros.h>
#include <plexdb/support/tracy/tracy.hpp>

export module plexdb.coroutine.base;

import plexdb.base;
import plexdb.coroutine.debug;

// ============================================================================
// Tracy fiber tracking
// ============================================================================
namespace plexdb::coroutine {
    inline constexpr bool tracy_enabled =
#ifdef PLEXDB_ENABLE_TRACY_PROFILER
        true;
#else
        false;
#endif

    struct TracyFiber {
        // "0x" + hex digits for pointer + NUL
        static constexpr int k_len = 2 + static_cast<int>(sizeof(void*)) * 2 + 1;
        char        name[k_len]          = {};
        const char* continuation_name    = nullptr;
    };

    // Stub used when tracy_enabled=false. Must expose the same member names and
    // conversions as TracyFiber / const char* so that if constexpr(tracy_enabled)
    // branches are syntactically valid.
    struct EmptyTracyFiber {
        char        name[1]           = {};
        const char* continuation_name = nullptr;
        EmptyTracyFiber() = default;
        EmptyTracyFiber(const char*) noexcept {}
        EmptyTracyFiber& operator=(const char*) noexcept { return *this; }
        operator const char*() const noexcept { return nullptr; }
        explicit operator bool() const noexcept { return false; }
    };

#ifdef PLEXDB_ENABLE_TRACY_PROFILER
    namespace {
        constexpr char k_hex_chars[] = "0123456789abcdef";
    }

    inline void write_hex_ptr(char* buf, const void* ptr) noexcept {
        auto v = reinterpret_cast<uintptr_t>(ptr);
        constexpr int n = static_cast<int>(sizeof(void*)) * 2;
        buf[0] = '0'; buf[1] = 'x';
        for (int i = n - 1; i >= 0; --i) {
            buf[2 + i] = k_hex_chars[v & 0xf];
            v >>= 4;
        }
        buf[2 + n] = '\0';
    }
#else
    inline void write_hex_ptr(char*, const void*) noexcept {}
#endif

    // Always declared so if constexpr(tracy_enabled) branches are well-formed.
    // In non-tracy builds the variable is never written or read at runtime.
    inline thread_local const char* g_current_tracy_fiber = nullptr;
}

export namespace plexdb::coroutine {
    // ========================================================================
    // Start
    //   Controls whether a Task suspends before its body executes (Lazy),
    //   or begins executing inline during construction (Eager).
    // ========================================================================
    enum class Start { Lazy, Eager };

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
            std::coroutine_handle<> continuation = std::noop_coroutine();
#ifdef PLEXDB_EXCEPTIONS
            std::exception_ptr exception;
#endif
            // @note zero bytes in release builds via [[no_unique_address]] + empty type.
            [[no_unique_address]]
            Conditional<debug::enabled, debug::Frame, debug::EmptyFrame> debug_frame;

            [[no_unique_address]]
            Conditional<tracy_enabled, TracyFiber, EmptyTracyFiber> tracy;

            Task get_return_object() noexcept {
                debug::capture_frame_from_stacktrace(debug_frame);
                auto h = std::coroutine_handle<promise_type>::from_promise(*this);
                if constexpr (tracy_enabled) {
                    write_hex_ptr(tracy.name, h.address());
                    if constexpr (S == Start::Eager) {
                        g_current_tracy_fiber = tracy.name;
                        TracyFiberEnter(tracy.name);
                    }
                }
                return Task{h};
            }

            auto initial_suspend() noexcept {
                if constexpr (S == Start::Lazy) return std::suspend_always{};
                else return std::suspend_never{};
            }

            auto final_suspend() noexcept {
                struct Awaiter {
                    bool await_ready() noexcept { return false; }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        debug::pop_frame(h.promise().debug_frame);

                        // Root tasks leave their fiber on completion; children share the parent's.
                        if constexpr (tracy_enabled) {
                            if (!h.promise().tracy.continuation_name && g_current_tracy_fiber) {
                                g_current_tracy_fiber = nullptr;
                                TracyFiberLeave;
                            }
                        }

                        return h.promise().continuation;
                    }
                    void await_resume() noexcept {}
                };
                return Awaiter{};
            }

            void return_value(T value) { result.emplace(move(value)); }
            void unhandled_exception() {
#ifdef PLEXDB_EXCEPTIONS
                exception = std::current_exception();
#endif
                if constexpr (debug::enabled) {
                    println("unhandled exception in coroutine, async stack:");
                    debug::print_async_stack();
                }
            }
        };

        bool await_ready() const noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            debug::push_frame(handle.promise().debug_frame);

            if constexpr (tracy_enabled) {
                handle.promise().tracy.continuation_name = g_current_tracy_fiber;
            }

            handle.promise().continuation = caller;
            return handle;
        }

        T await_resume() {
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception)
                std::rethrow_exception(handle.promise().exception);
#endif
            return move(handle.promise().result.value());
        }

        // ====================================================================
        // manual api
        // ====================================================================
        void resume() {
            if constexpr (tracy_enabled) {
                g_current_tracy_fiber = handle.promise().tracy.name;
                TracyFiberEnter(handle.promise().tracy.name);
            }
            handle.resume();
        }
        bool done() const { return handle.done(); }
        bool has_value() const { return handle.promise().result.has_value(); }
        T& value() & {
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
#endif
            assert_true(has_value(), "coroutine result is not ready");
            return handle.promise().result.value();
        }
        const T& value() const & {
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
#endif
            assert_true(has_value(), "coroutine result is not ready");
            return handle.promise().result.value();
        }

        // ====================================================================
        // constructors/destructors
        // ====================================================================
        Task(Task&& other) noexcept : handle(other.handle) { other.handle = nullptr; }

        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                destroy();
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        ~Task() { destroy(); }

    private:
        std::coroutine_handle<promise_type> handle;
        explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        void destroy() { if (handle) { handle.destroy(); handle = nullptr; } }
    };

    template<Start S>
    struct [[nodiscard]] Task<void, S> {
        // ====================================================================
        // built-in api (e.g. co_await)
        // ====================================================================
        struct promise_type {
            std::coroutine_handle<> continuation = std::noop_coroutine();
#ifdef PLEXDB_EXCEPTIONS
            std::exception_ptr exception;
#endif
            [[no_unique_address]]
            Conditional<debug::enabled, debug::Frame, debug::EmptyFrame> debug_frame;

            [[no_unique_address]]
            Conditional<tracy_enabled, TracyFiber, EmptyTracyFiber> tracy;

            Task get_return_object() noexcept {
                debug::capture_frame_from_stacktrace(debug_frame);
                auto h = std::coroutine_handle<promise_type>::from_promise(*this);
                if constexpr (tracy_enabled) {
                    write_hex_ptr(tracy.name, h.address());
                    // Eager tasks start executing immediately during construction with no
                    // prior Task::resume() call, so enter the fiber here instead.
                    if constexpr (S == Start::Eager) {
                        g_current_tracy_fiber = tracy.name;
                        TracyFiberEnter(tracy.name);
                    }
                }
                return Task{h};
            }

            auto initial_suspend() noexcept {
                if constexpr (S == Start::Lazy) return std::suspend_always{};
                else return std::suspend_never{};
            }

            auto final_suspend() noexcept {
                struct Awaiter {
                    // @note coroutine pauses after final_suspend, meaning ~Task needs to call destroy
                    bool await_ready() noexcept { return false; }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        debug::pop_frame(h.promise().debug_frame);

                        // Root tasks leave their fiber on completion; children share the parent's.
                        if constexpr (tracy_enabled) {
                            if (!h.promise().tracy.continuation_name && g_current_tracy_fiber) {
                                g_current_tracy_fiber = nullptr;
                                TracyFiberLeave;
                            }
                        }

                        return h.promise().continuation;
                    }
                    void await_resume() noexcept {}
                };
                return Awaiter{};
            }

            void return_void() noexcept {}
            void unhandled_exception() {
#ifdef PLEXDB_EXCEPTIONS
                exception = std::current_exception();
#endif
                if constexpr (debug::enabled) {
                    println("unhandled exception in coroutine, async stack:");
                    debug::print_async_stack();
                }
            }
        };

        bool await_ready() const noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            debug::push_frame(handle.promise().debug_frame);

            if constexpr (tracy_enabled) {
                handle.promise().tracy.continuation_name = g_current_tracy_fiber;
            }

            handle.promise().continuation = caller;
            return handle;
        }

        void await_resume() {
#ifdef PLEXDB_EXCEPTIONS
            if (handle.promise().exception)
                std::rethrow_exception(handle.promise().exception);
#endif
        }

        // ====================================================================
        // manual api
        // ====================================================================
        void resume() {
            if constexpr (tracy_enabled) {
                g_current_tracy_fiber = handle.promise().tracy.name;
                TracyFiberEnter(handle.promise().tracy.name);
            }

            handle.resume();
        }
        bool done() const { return handle.done(); }

        // ====================================================================
        // constructors/destructors
        // ====================================================================
        Task(Task&& other) noexcept : handle(other.handle) { other.handle = nullptr; }

        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                destroy();
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        ~Task() { destroy(); }

    private:
        std::coroutine_handle<promise_type> handle;
        explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        void destroy() { if (handle) { handle.destroy(); handle = nullptr; } }
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
    struct NoOp { void operator()() const noexcept {} };

    template<typename OnSuspend, typename OnResume, typename OnDelete = NoOp>
        requires TriviallyCopyable<OnDelete>
    struct [[nodiscard]] Awaitable {
        OnSuspend on_suspend;
        OnResume  on_resume;
        OnDelete  on_delete;
        bool      _resumed = false;

        Awaitable(OnSuspend s, OnResume r)              : on_suspend(move(s)), on_resume(move(r))               {}
        Awaitable(OnSuspend s, OnResume r, OnDelete d)  : on_suspend(move(s)), on_resume(move(r)), on_delete(move(d)) {}

        bool await_ready() noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            debug::save_frame(_saved_frame);

            if constexpr (tracy_enabled) {
                _saved_tracy_fiber = g_current_tracy_fiber;
                if (g_current_tracy_fiber) {
                    g_current_tracy_fiber = nullptr;
                    TracyFiberLeave;
                }
            }

            on_suspend(h);
        }

        decltype(auto) await_resume() noexcept {
            _resumed = true;

            debug::restore_frame(_saved_frame);

            if constexpr (tracy_enabled) {
                if (_saved_tracy_fiber) {
                    g_current_tracy_fiber = _saved_tracy_fiber;
                    TracyFiberEnter(_saved_tracy_fiber);
                }
            }

            return on_resume();
        }

        ~Awaitable() { if (!_resumed) on_delete(); }

        [[no_unique_address]]
        Conditional<debug::enabled, debug::FrameLink, debug::EmptyFrame> _saved_frame{};

        [[no_unique_address]]
        Conditional<tracy_enabled, const char*, EmptyTracyFiber> _saved_tracy_fiber{};
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

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }

            std::suspend_always yield_value(T value) {
                current.emplace(move(value));
                return {};
            }

            void return_void() noexcept {}
            void unhandled_exception() { }
        };

        struct Iterator {
            std::coroutine_handle<promise_type> handle;

            Iterator& operator++() {
                assert_true(handle && !handle.done(), "incremented past-end generator iterator");
                handle.resume();
                if (handle.done()) handle = nullptr;
                return *this;
            }

            T& operator*() const { return handle.promise().current.value(); }
            T* operator->() const { return &handle.promise().current.value(); }

            bool operator==(const Iterator& other) const noexcept {
                return handle == other.handle;
            }

            bool operator!=(const Iterator& other) const noexcept {
                return handle != other.handle;
            }
        };

        Iterator begin() {
            if (handle) handle.resume();
            if (!handle || handle.done()) return end();
            return Iterator{handle};
        }

        Iterator end() noexcept { return Iterator{nullptr}; }

        // ====================================================================
        // manual api
        // ====================================================================
        Optional<T> next() {
            if (!handle || handle.done()) return {};
            handle.resume();
            if (handle.done()) return {};
            return Optional<T>{move(handle.promise().current.value())};
        }

        // ====================================================================
        // constructors/destructors
        // ====================================================================
        Generator(Generator&& other) noexcept : handle(other.handle) { other.handle = nullptr; }

        Generator& operator=(Generator&& other) noexcept {
            if (this != &other) {
                destroy();
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        Generator(const Generator&) = delete;
        Generator& operator=(const Generator&) = delete;
        ~Generator() { destroy(); }

    private:
        std::coroutine_handle<promise_type> handle;
        explicit Generator(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        void destroy() { if (handle) { handle.destroy(); handle = nullptr; } }
    };
}
