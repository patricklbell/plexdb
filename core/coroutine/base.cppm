module;
#include <coroutine>
#include "macros.h"

export module plexdb.coroutine.base;

import plexdb.base;

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
    struct Task;

    template<typename T, Start S>
    struct Task {
        // ====================================================================
        // built-in api (e.g. co_await)
        // ====================================================================
        struct promise_type {
            Optional<T> result;
            std::coroutine_handle<> continuation = std::noop_coroutine();

            Task get_return_object() noexcept {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            auto initial_suspend() noexcept {
                if constexpr (S == Start::Lazy) return std::suspend_always{};
                else return std::suspend_never{};
            }

            auto final_suspend() noexcept {
                struct Awaiter {
                    bool await_ready() noexcept { return false; }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        return h.promise().continuation;
                    }
                    void await_resume() noexcept {}
                };
                return Awaiter{};
            }

            void return_value(T value) { result.emplace(move(value)); }
            void unhandled_exception() { }
        };

        bool await_ready() const noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle;
        }

        T await_resume() { return move(handle.promise().result.value()); }

        // ====================================================================
        // manual api
        // ====================================================================
        void resume() { handle.resume(); }
        bool done() const { return handle.done(); }
        bool has_value() const { return handle.promise().result.has_value(); }
        T& value() & { assert_true(has_value(), "coroutine result is not ready"); return handle.promise().result.value(); }
        const T& value() const & { assert_true(has_value(), "coroutine result is not ready"); return handle.promise().result.value(); }

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
    struct Task<void, S> {
        // ====================================================================
        // built-in api (e.g. co_await)
        // ====================================================================
        struct promise_type {
            std::coroutine_handle<> continuation = std::noop_coroutine();

            Task get_return_object() noexcept {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
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
                        return h.promise().continuation;
                    }
                    void await_resume() noexcept {}
                };
                return Awaiter{};
            }

            void return_void() noexcept {}
            void unhandled_exception() { }
        };

        bool await_ready() const noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle;
        }

        void await_resume() noexcept {}

        // ====================================================================
        // manual api
        // ====================================================================
        void resume() { handle.resume(); }
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

    template<typename T>
    T drive(coroutine::Task<T> task) {
        task.resume();
        while (!task.done()) { task.resume(); }
        return move(task.value());
    }
    inline void drive(coroutine::Task<void> task) {
        task.resume();
        while (!task.done()) { task.resume(); }
    }

    // ========================================================================
    // Awaitable<OnSuspend, OnResume>
    //   Generic bridge between a coroutine and an external async event source
    //   (e.g. io_uring, timers, inter-thread signals).
    //
    //   OnSuspend: void(std::coroutine_handle<>)
    //     Called when the coroutine suspends at co_await. Store the handle and
    //     submit the async operation. The event loop resumes the coroutine by
    //     calling handle.resume() when the completion arrives.
    //
    //   OnResume: Result()
    //     Called to extract the result after the coroutine is woken. May be
    //     void for fire-and-forget operations.
    //
    //   Example (io_uring read inside a Task coroutine):
    //     int n = co_await Awaitable{
    //         [&](std::coroutine_handle<> h) {
    //             pending_reads[fd] = h;
    //             sqe_push_read(ring, fd, buf_idx);
    //         },
    //         [&]() -> int { return bytes_read; }
    //     };
    // ========================================================================
    template<typename OnSuspend, typename OnResume>
    struct Awaitable {
        OnSuspend on_suspend;
        OnResume  on_resume;

        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept { on_suspend(h); }
        decltype(auto) await_resume() noexcept { return on_resume(); }
    };

    template<typename OnSuspend, typename OnResume>
    Awaitable(OnSuspend, OnResume) -> Awaitable<OnSuspend, OnResume>;

    // ========================================================================
    // generator
    //   @note lazy
    // ========================================================================
    template<typename T>
    struct Generator {
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
