#include <catch2/catch_test_macros.hpp>
#include <coroutine>

import plexdb.base;
import plexdb.coroutine;

using namespace plexdb;
using namespace plexdb::coroutine;

// ============================================================================
// Task (lazy, default)
// ============================================================================

TEST_CASE("Task<int> returns value", "[coroutine]") {
    auto task = []() -> Task<int> {
        co_return 42;
    }();
    task.resume();
    REQUIRE(task.done());
    REQUIRE(task.has_value());
    REQUIRE(task.value() == 42);
}

TEST_CASE("Task<void> completes", "[coroutine]") {
    bool executed = false;
    auto task     = [&]() -> Task<> {
        executed = true;
        co_return;
    }();
    REQUIRE_FALSE(executed);
    task.resume();
    REQUIRE(executed);
    REQUIRE(task.done());
}

TEST_CASE("Task chaining with co_await", "[coroutine]") {
    auto inner = []() -> Task<int> {
        co_return 10;
    };
    auto outer = [&]() -> Task<int> {
        auto a = co_await inner();
        auto b = co_await inner();
        co_return a + b;
    }();
    outer.resume();
    REQUIRE(outer.done());
    REQUIRE(outer.value() == 20);
}

TEST_CASE("Task deep chain with symmetric transfer", "[coroutine]") {
    auto leaf = []() -> Task<int> {
        co_return 1;
    };
    auto mid = [&]() -> Task<int> {
        auto v = co_await leaf();
        co_return v + 1;
    };
    auto root = [&]() -> Task<int> {
        auto v = co_await mid();
        co_return v + 1;
    };

    auto task = root();
    task.resume();
    REQUIRE(task.done());
    REQUIRE(task.value() == 3);
}

TEST_CASE("Task move semantics", "[coroutine]") {
    auto make = []() -> Task<int> {
        co_return 7;
    };
    auto t1 = make();
    auto t2 = move(t1);
    t2.resume();
    REQUIRE(t2.done());
    REQUIRE(t2.value() == 7);
}

TEST_CASE("Task<void> chaining", "[coroutine]") {
    int  counter = 0;
    auto step    = [&]() -> Task<> {
        counter++;
        co_return;
    };
    auto pipeline = [&]() -> Task<> {
        co_await step();
        co_await step();
        co_await step();
    }();
    pipeline.resume();
    REQUIRE(pipeline.done());
    REQUIRE(counter == 3);
}

// ============================================================================
// Task (eager)
// ============================================================================

TEST_CASE("Task<int, Start::Eager> runs immediately", "[coroutine]") {
    bool executed = false;
    auto task     = [&]() -> Task<int, Start::Eager> {
        executed = true;
        co_return 99;
    }();
    REQUIRE(executed);
    REQUIRE(task.done());
    REQUIRE(task.has_value());
    REQUIRE(task.value() == 99);
}

TEST_CASE("Task<void, Start::Eager> runs immediately", "[coroutine]") {
    bool executed = false;
    auto task     = [&]() -> Task<void, Start::Eager> {
        executed = true;
        co_return;
    }();
    REQUIRE(executed);
    REQUIRE(task.done());
}

TEST_CASE("Task<int, Start::Eager> co_await from lazy task", "[coroutine]") {
    // Eager sub-task completes synchronously; lazy outer resumes it via await_ready.
    auto sub = []() -> Task<int, Start::Eager> {
        co_return 5;
    };
    auto outer = [&]() -> Task<int> {
        auto v = co_await sub();
        co_return v * 2;
    }();
    outer.resume();
    REQUIRE(outer.done());
    REQUIRE(outer.value() == 10);
}

// ============================================================================
// Awaitable — generic bridge to async event sources
// ============================================================================

TEST_CASE("Awaitable suspends and resumes with result", "[coroutine]") {
    // Simulate an event loop: store the handle, then drive it externally.
    std::coroutine_handle<> stored;
    int                     result = 0;

    auto task = [&]() -> Task<int> {
        int v = co_await Awaitable{
            [&](std::coroutine_handle<> h) { stored = h; },
            [&]() -> int {
                return result;
            }};
        co_return v * 2;
    }();

    task.resume();
    REQUIRE_FALSE(task.done());
    REQUIRE(stored);

    result = 21;
    stored.resume(); // simulates event loop processing a CQE
    REQUIRE(task.done());
    REQUIRE(task.value() == 42);
}

TEST_CASE("Awaitable<void> fire-and-forget", "[coroutine]") {
    std::coroutine_handle<> stored;
    bool                    completed = false;

    auto task = [&]() -> Task<> {
        co_await Awaitable{
            [&](std::coroutine_handle<> h) { stored = h; },
            [&]() {
                completed = true;
            }};
    }();

    task.resume();
    REQUIRE_FALSE(task.done());

    stored.resume();
    REQUIRE(completed);
    REQUIRE(task.done());
}

// ============================================================================
// Generator
// ============================================================================

TEST_CASE("Generator yields values via range-for", "[coroutine]") {
    auto gen = []() -> Generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    }();

    int sum = 0;
    for (auto& val : gen) {
        sum += val;
    }
    REQUIRE(sum == 6);
}

TEST_CASE("Generator next() API", "[coroutine]") {
    auto gen = []() -> Generator<int> {
        co_yield 10;
        co_yield 20;
    }();

    auto a = gen.next();
    REQUIRE(a.has_value());
    REQUIRE(a.value() == 10);

    auto b = gen.next();
    REQUIRE(b.has_value());
    REQUIRE(b.value() == 20);

    auto c = gen.next();
    REQUIRE_FALSE(c.has_value());
}

TEST_CASE("Generator empty", "[coroutine]") {
    auto gen = []() -> Generator<int> {
        co_return;
    }();

    auto val = gen.next();
    REQUIRE_FALSE(val.has_value());

    int  count = 0;
    auto gen2  = []() -> Generator<int> {
        co_return;
    }();
    for (auto& v : gen2) {
        (void)v;
        count++;
    }
    REQUIRE(count == 0);
}

TEST_CASE("Generator sequence with parameter", "[coroutine]") {
    auto range = [](int start, int end) -> Generator<int> {
        for (int i = start; i < end; i++) {
            co_yield i;
        }
    };

    int expected = 0;
    for (auto& val : range(0, 5)) {
        REQUIRE(val == expected);
        expected++;
    }
    REQUIRE(expected == 5);
}

TEST_CASE("Generator early destruction", "[coroutine]") {
    bool destroyed = false;
    struct Guard {
        bool& flag;
        ~Guard() {
            flag = true;
        }
    };

    {
        auto gen = [&]() -> Generator<int> {
            Guard g{destroyed};
            co_yield 1;
            co_yield 2;
        }();
        auto val = gen.next();
        REQUIRE(val.has_value());
        REQUIRE(val.value() == 1);
    }
    REQUIRE(destroyed);
}
