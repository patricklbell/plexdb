module;
#include <coroutine>
#include "macros.h"

export module plexdb.coroutine;

import plexdb.base;

export namespace plexdb::coroutine {
    // ========================================================================
    // coroutine handle - opaque wrapper hiding std::coroutine_handle
    // ========================================================================
    struct CoroutineHandle {
        void* address = nullptr;

        void resume() const {
            assert_true(address != nullptr, "resumed null coroutine");
            std::coroutine_handle<>::from_address(address).resume();
        }

        bool done() const {
            if (!address) return true;
            return std::coroutine_handle<>::from_address(address).done();
        }

        explicit operator bool() const noexcept { return address != nullptr; }
    };

    // ========================================================================
    // task - lazy awaitable coroutine
    // ========================================================================
    template<typename T = void>
    struct Task;

    template<typename T>
    struct Task {
        struct Promise {
            Optional<T> result;
            std::coroutine_handle<> continuation = std::noop_coroutine();

            Task get_return_object() noexcept {
                return Task{std::coroutine_handle<Promise>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            auto final_suspend() noexcept {
                struct Awaiter {
                    bool await_ready() noexcept { return false; }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
                        return h.promise().continuation;
                    }
                    void await_resume() noexcept {}
                };
                return Awaiter{};
            }

            void return_value(T value) { result.emplace(move(value)); }
            void unhandled_exception() { PLEXDB_TRAP; }
        };

        using promise_type = Promise;

        // -- awaiter interface (compiler-facing for co_await) --
        bool await_ready() const noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle;
        }

        T await_resume() { return move(handle.promise().result.value()); }

        // -- manual driving API --
        void resume() { handle.resume(); }
        bool done() const { return handle.done(); }
        bool has_value() const { return handle.promise().result.has_value(); }
        T& value() & { return handle.promise().result.value(); }
        const T& value() const & { return handle.promise().result.value(); }

        // -- lifecycle --
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
        std::coroutine_handle<Promise> handle;
        explicit Task(std::coroutine_handle<Promise> h) noexcept : handle(h) {}
        void destroy() { if (handle) { handle.destroy(); handle = nullptr; } }
    };

    template<>
    struct Task<void> {
        struct Promise {
            std::coroutine_handle<> continuation = std::noop_coroutine();

            Task get_return_object() noexcept {
                return Task{std::coroutine_handle<Promise>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            auto final_suspend() noexcept {
                struct Awaiter {
                    bool await_ready() noexcept { return false; }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
                        return h.promise().continuation;
                    }
                    void await_resume() noexcept {}
                };
                return Awaiter{};
            }

            void return_void() noexcept {}
            void unhandled_exception() { PLEXDB_TRAP; }
        };

        using promise_type = Promise;

        bool await_ready() const noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle;
        }

        void await_resume() noexcept {}

        void resume() { handle.resume(); }
        bool done() const { return handle.done(); }

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
        std::coroutine_handle<Promise> handle;
        explicit Task(std::coroutine_handle<Promise> h) noexcept : handle(h) {}
        void destroy() { if (handle) { handle.destroy(); handle = nullptr; } }
    };

    // ========================================================================
    // generator - lazy pull-based sequence
    // ========================================================================
    template<typename T>
    struct Generator {
        struct Promise {
            Optional<T> current;

            Generator get_return_object() noexcept {
                return Generator{std::coroutine_handle<Promise>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }

            std::suspend_always yield_value(T value) {
                current.emplace(move(value));
                return {};
            }

            void return_void() noexcept {}
            void unhandled_exception() { PLEXDB_TRAP; }
        };

        using promise_type = Promise;

        struct Iterator {
            std::coroutine_handle<Promise> handle;

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

        Optional<T> next() {
            if (!handle || handle.done()) return {};
            handle.resume();
            if (handle.done()) return {};
            return Optional<T>{move(handle.promise().current.value())};
        }

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
        std::coroutine_handle<Promise> handle;
        explicit Generator(std::coroutine_handle<Promise> h) noexcept : handle(h) {}
        void destroy() { if (handle) { handle.destroy(); handle = nullptr; } }
    };
}
