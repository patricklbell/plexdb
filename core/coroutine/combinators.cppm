module;
#include <coroutine>
#include <new>
#include <plexdb/macros/macros.h>

export module plexdb.coroutine.combinators;

import plexdb.base;
import plexdb.arena;
import plexdb.threads;
import plexdb.coroutine.base;

export namespace plexdb::coroutine {
    // ========================================================================
    // Latch
    //   Fan-in: N events -> one wake. Not a mutual-exclusion/FIFO primitive
    //   (see aio.cpp's buf_waiters / pager.cpp's tx_waiters for that shape).
    // ========================================================================
    struct Latch {
        U64                     remaining;
        std::coroutine_handle<> waiter{};

        // @note callers must transfer via the returned handle (from an
        // await_suspend), not call .resume() on it: the last arrival is
        // typically still on the call stack, and a direct resume there lets
        // the woken waiter destroy that same still-executing frame.
        std::coroutine_handle<> arrive() noexcept {
            assert_true(remaining > 0, "no remaining count in coroutine latch");
            if (--remaining == 0 && waiter) {
                auto w = waiter;
                waiter = {};
                return w;
            }
            return std::noop_coroutine();
        }
    };

    // What actually gets resumed for each fanned-out slot. Not the generic
    // Task<T, Start::Eager>: its final_suspend must arrive() the latch and
    // transfer into the waiter itself, which nobody co_awaits a Runner to set up.
    struct Runner {
        struct promise_type {
            Latch* latch = nullptr;

            template<typename T>
            promise_type(Task<T>&, Latch& l) noexcept
                : latch(&l) {
            }

            Runner get_return_object() noexcept {
                return Runner{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_never initial_suspend() noexcept {
                return {};
            }

            struct FinalAwaiter {
                bool await_ready() noexcept {
                    return false;
                }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    return h.promise().latch->arrive();
                }
                void await_resume() noexcept {
                }
            };
            FinalAwaiter final_suspend() noexcept {
                return {};
            }

            void return_void() noexcept {
            }
            // @note swallows the task's exception: nobody observes a
            // fired-and-forgotten runner's result, so a throwing task still
            // lets the batch's joiner complete.
            void unhandled_exception() noexcept {
            }
        };

        std::coroutine_handle<promise_type> handle;

        explicit Runner(std::coroutine_handle<promise_type> h) noexcept
            : handle(h) {
        }
        Runner(Runner&& other) noexcept
            : handle(other.handle) {
            other.handle = nullptr;
        }
        Runner(const Runner&) = delete;
        ~Runner() {
            if (handle) {
                handle.destroy();
                handle = nullptr;
            }
        }
    };

    // Eager: starts running as part of construction, so `co_await t` below is
    // the FIRST touch of `t` — the only point a fresh Lazy Task may legally be
    // resumed (re-resuming/re-awaiting an already-started Task is undefined).
    template<typename T>
    Runner run_and_arrive(Task<T> t, [[maybe_unused]] Latch& latch) {
        co_await t;
    }

    // Caller-owned storage, no allocation: `tasks` is a view over N
    // already-constructed, fresh (never-resumed, never-awaited) Lazy tasks.
    // `runners` is separate raw storage of the same length, one Runner
    // placement-constructed per fired task.
    //
    // @note runners use separate storage rather than reusing tasks[] in place:
    // every Task<T, S> is a single coroutine_handle, so the sizes match, but
    // accessing an object through a pointer of its old static type after a
    // destroy-then-placement-new swap needs std::launder to be defined.
    //
    // @note every task is fired at once (each runs synchronously up to its
    // first suspension point as it's constructed below), not staged through a
    // concurrency window: the I/O layer beneath already applies backpressure
    // (e.g. an io_uring buffer pool's wait queue) and batches the resulting
    // submissions, so no cap belongs here.
    template<typename T>
    Task<> when_all(TArrayView<Task<T>> tasks, TArrayView<Runner> runners) {
        assert_true(runners.length >= tasks.length, "runners view smaller than tasks view");

        if (tasks.length == 0) {
            co_return;
        }

        Latch latch{tasks.length};
        for (U64 task_idx = 0; task_idx < tasks.length; task_idx++) {
            new (&runners.ptr[task_idx]) Runner(run_and_arrive(move(tasks.ptr[task_idx]), latch));
        }

        struct RunnerGuard {
            TArrayView<Runner> runners;
            ~RunnerGuard() {
                for (Runner& runner : runners) {
                    runner.~Runner();
                }
            }
        } guard{runners};

        if (latch.remaining > 0) {
            co_await Awaitable{
                [l = &latch](std::coroutine_handle<> h) { l->waiter = h; },
                []() {},
                // @note mirrors on_suspend: if destroyed while parked here,
                // clear `waiter` first so a still-outstanding runner's eventual
                // arrive() can't transfer into a frame being torn down.
                [l = &latch]() {
                    l->waiter = {};
                }
            };
        }
    }

    // @note uses scratch. fn must return Task<> (void) — every current caller's
    // per-item work has no result to collect, only completion to wait for.
    template<typename Item, typename Fn>
    Task<> when_all(TArrayView<Item> items, Fn&& fn) {
        threads::Scope     sc             = threads::scratch();
        coroutine::Task<>* task_storage   = arena::push_array_no_zero<coroutine::Task<>>(*sc.arena, items.length);
        coroutine::Runner* runner_storage = arena::push_array_no_zero<coroutine::Runner>(*sc.arena, items.length);

        TArrayView<coroutine::Task<>> tasks{task_storage, items.length};
        TArrayView<coroutine::Runner> runners{runner_storage, items.length};

        if (items.length == 0) {
            co_return;
        }

        for (U64 item_idx = 0; item_idx < items.length; item_idx++) {
            new (&tasks.ptr[item_idx]) Task<>(fn(items.ptr[item_idx]));
        }

        co_await when_all(tasks, runners);

        for (coroutine::Task<>& task : tasks) {
            task.~Task();
        }
    }
}
