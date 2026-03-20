#include <catch2/catch_test_macros.hpp>

#include "plexdb_coro.h"
#include <vector>

import plexdb.base;
import plexdb.coro;

using namespace plexdb;
using namespace plexdb::coro;

namespace {
    // Helper: a simple synchronous awaitable that completes immediately.
    struct ImmediateAwaitable : IoAwaitable {
        ImmediateAwaitable() : IoAwaitable(IoAwaitable::default_complete) {}
    };
}

TEST_CASE("Task completes without suspension", "[plexdb.coro]") {
    int counter = 0;
    auto make_task = [&]() -> Task {
        counter++;
        co_return;
    };

    Task t = make_task();
    REQUIRE(!t.done());
    t.start();
    REQUIRE(t.done());
    REQUIRE(counter == 1);
}

TEST_CASE("Task suspends and resumes on IoAwaitable", "[plexdb.coro]") {
    IoAwaitable aw;
    int step = 0;

    auto make_task = [&]() -> Task {
        step = 1;
        int result = co_await aw;
        step = 2 + result;
    };

    Task t = make_task();
    t.start();

    REQUIRE(step == 1);
    REQUIRE(!t.done());

    aw.complete(10);

    REQUIRE(t.done());
    REQUIRE(step == 12); // 2 + 10
}

TEST_CASE("Task move semantics", "[plexdb.coro]") {
    auto make_task = []() -> Task { co_return; };

    Task a = make_task();
    Task b = plexdb::move(a);

    CHECK(a.done());
    CHECK(!b.done());

    b.start();
    CHECK(b.done());
}

TEST_CASE("Task co_awaited from another Task", "[plexdb.coro]") {
    int order = 0;
    IoAwaitable aw;

    auto inner = [&]() -> Task {
        order = 1;
        co_await aw;
        order = 3;
    };

    auto outer = [&](Task inner_task) -> Task {
        order = 0;
        co_await inner_task;
        order = 4;
    };

    Task i = inner();
    Task o = outer(plexdb::move(i));
    o.start();

    // outer started inner, inner suspended at aw
    REQUIRE(order == 1);
    REQUIRE(!o.done());

    aw.complete(0);

    // inner finished, outer resumed and also finished
    REQUIRE(order == 4);
    REQUIRE(o.done());
}

TEST_CASE("IoAwaitable custom complete_fn", "[plexdb.coro]") {
    static int side_effect = 0;

    struct CustomAwaitable : IoAwaitable {
        static void on_complete(IoAwaitable* self, int result) noexcept {
            side_effect = result * 2;
            default_complete(self, result);
        }
        CustomAwaitable() : IoAwaitable(on_complete) {}
    };

    CustomAwaitable aw;
    int got = -1;

    auto make_task = [&]() -> Task {
        got = co_await aw;
    };

    Task t = make_task();
    t.start();

    side_effect = 0;
    aw.complete(7);

    REQUIRE(got == 7);
    REQUIRE(side_effect == 14);
    REQUIRE(t.done());
}

TEST_CASE("EventLoop tracks in-flight count", "[plexdb.coro]") {
    EventLoop loop{nullptr, 4};
    REQUIRE(loop.can_submit());
    loop.in_flight = 4;
    REQUIRE(!loop.can_submit());
    loop.in_flight = 3;
    REQUIRE(loop.can_submit());
}
