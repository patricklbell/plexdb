module;
#include "macros.h"
#include <coroutine>

export module plexdb.coro;

import plexdb.base;
import plexdb.os;

export namespace plexdb::coro {
    // ========================================================================
    // IoAwaitable
    //   Base type for all coroutine-awaitable IO operations.
    //   The awaitable lives in the coroutine frame; its address is stored in
    //   io_uring user_data so the event loop can resume the coroutine on
    //   completion.
    //
    //   Subclasses override complete_fn to run type-specific teardown before
    //   resuming (e.g. buffer release after a send).
    // ========================================================================
    struct IoAwaitable {
        using CompleteFn = void(*)(IoAwaitable* self, int result) noexcept;

        CompleteFn              complete_fn;
        std::coroutine_handle<> continuation = {};
        int                     result       = 0;

        explicit IoAwaitable(CompleteFn fn = default_complete) noexcept
            : complete_fn(fn) {}

        bool await_ready()                          const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept { continuation = h; }
        int  await_resume()                               noexcept { return result; }

        // Called from the event loop when the IO operation completes.
        void complete(int res) noexcept { complete_fn(this, res); }

        static void default_complete(IoAwaitable* self, int result) noexcept {
            self->result = result;
            if (self->continuation)
                self->continuation.resume();
        }
    };

    // ========================================================================
    // Task
    //   Lightweight C++20 stackless coroutine handle.
    //   Starts suspended; use start() to begin execution or co_await to chain.
    // ========================================================================
    struct Task {
        struct promise_type {
            std::coroutine_handle<> continuation = {};

            Task get_return_object() noexcept {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept
                {
                    if (auto c = h.promise().continuation)
                        return c;
                    return std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };

            FinalAwaiter final_suspend() noexcept { return {}; }
            void return_void()          noexcept {}
            void unhandled_exception()  noexcept {}
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;

        Task() = default;
        explicit Task(Handle h) noexcept : handle(h) {}
        ~Task() { if (handle) handle.destroy(); }

        Task(const Task&)            = delete;
        Task& operator=(const Task&) = delete;

        Task(Task&& o) noexcept : handle(exchange(o.handle, {})) {}
        Task& operator=(Task&& o) noexcept {
            if (this != &o) {
                if (handle) handle.destroy();
                handle = exchange(o.handle, {});
            }
            return *this;
        }

        bool done() const noexcept { return !handle || handle.done(); }

        // Run the coroutine until the next suspension point or completion.
        void start() noexcept { if (handle && !handle.done()) handle.resume(); }

        // co_await support: run this task as a subtask of another coroutine.
        bool await_ready()                          const noexcept { return done(); }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            handle.promise().continuation = h;
            handle.resume();
        }
        void await_resume() noexcept {}
    };

    // ========================================================================
    // EventLoop
    //   Tracks in-flight IO operations submitted through io_uring.
    //   Does not own a ring; the ring is owned by the calling event loop
    //   (e.g. listen_coro).  max_in_flight caps the number of outstanding
    //   io_uring SQEs from coroutines to bound memory usage and latency
    //   variance.
    // ========================================================================
    constexpr U32 DEFAULT_MAX_IN_FLIGHT = 256;

    struct EventLoop {
        void* ring_ptr      = nullptr; // uring::Ring* kept opaque to avoid header order issues
        U32   max_in_flight = DEFAULT_MAX_IN_FLIGHT;
        U32   in_flight     = 0;

        EventLoop() = default;
        explicit EventLoop(void* ring, U32 max_in_flight = DEFAULT_MAX_IN_FLIGHT) noexcept
            : ring_ptr(ring), max_in_flight(max_in_flight) {}

        bool can_submit() const noexcept { return in_flight < max_in_flight; }
    };

} // namespace plexdb::coro
