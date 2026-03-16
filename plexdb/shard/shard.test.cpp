#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <atomic>

import plexdb.base;
import plexdb.shard;

using namespace plexdb;
using namespace plexdb::shard;

// ============================================================================
// SPSC queue
// ============================================================================
TEST_CASE("spsc queue basic push/pop", "[plexdb.shard.spsc]") {
    SpscQueue<U64, 4> q;

    SECTION("empty queue pop fails") {
        U64 val;
        REQUIRE(!try_pop(q, val));
        REQUIRE(size(q) == 0);
    }

    SECTION("single push/pop") {
        REQUIRE(try_push(q, 42_u64));
        REQUIRE(size(q) == 1);

        U64 val;
        REQUIRE(try_pop(q, val));
        REQUIRE(val == 42);
        REQUIRE(size(q) == 0);
    }

    SECTION("fill to capacity") {
        for (U64 i = 0; i < 4; i++)
            REQUIRE(try_push(q, i));
        REQUIRE(!try_push(q, 99_u64));
        REQUIRE(size(q) == 4);

        for (U64 i = 0; i < 4; i++) {
            U64 val;
            REQUIRE(try_pop(q, val));
            REQUIRE(val == i);
        }
        REQUIRE(size(q) == 0);
    }

    SECTION("wraparound") {
        for (U64 i = 0; i < 4; i++)
            REQUIRE(try_push(q, i));
        
        U64 val;
        for (U64 i = 0; i < 4; i++)
            REQUIRE(try_pop(q, val));

        for (U64 i = 10; i < 14; i++)
            REQUIRE(try_push(q, i));

        for (U64 i = 10; i < 14; i++) {
            REQUIRE(try_pop(q, val));
            REQUIRE(val == i);
        }
    }
}

TEST_CASE("spsc queue FIFO ordering", "[plexdb.shard.spsc]") {
    SpscQueue<U64, 16> q;

    for (U64 i = 0; i < 16; i++)
        REQUIRE(try_push(q, i * 7));

    for (U64 i = 0; i < 16; i++) {
        U64 val;
        REQUIRE(try_pop(q, val));
        REQUIRE(val == i * 7);
    }
}

TEST_CASE("spsc queue concurrent producer/consumer", "[plexdb.shard.spsc]") {
    constexpr U64 COUNT = 100000;
    SpscQueue<U64, 1024> q;
    std::atomic<bool> done{false};

    std::thread producer([&q, &done]() {
        for (U64 i = 0; i < COUNT; i++) {
            while (!try_push(q, i)) {}
        }
        done.store(true, std::memory_order_release);
    });

    U64 expected = 0;
    while (expected < COUNT) {
        U64 val;
        if (try_pop(q, val)) {
            REQUIRE(val == expected);
            expected++;
        }
    }

    producer.join();
    REQUIRE(expected == COUNT);
    REQUIRE(size(q) == 0);
}

TEST_CASE("spsc queue with struct type", "[plexdb.shard.spsc]") {
    struct Msg {
        U32 source;
        U32 id;
        U64 payload;
    };

    SpscQueue<Msg, 8> q;
    REQUIRE(try_push(q, Msg{.source=1, .id=42, .payload=0xDEAD}));

    Msg out;
    REQUIRE(try_pop(q, out));
    REQUIRE(out.source == 1);
    REQUIRE(out.id == 42);
    REQUIRE(out.payload == 0xDEAD);
}

// ============================================================================
// Token mapping
// ============================================================================
TEST_CASE("token_of deterministic", "[plexdb.shard.token]") {
    U8 key[] = "hello";
    U64 t1 = token_of(key, 5);
    U64 t2 = token_of(key, 5);
    REQUIRE(t1 == t2);

    U8 key2[] = "world";
    U64 t3 = token_of(key2, 5);
    REQUIRE(t1 != t3);
}

TEST_CASE("owning_shard uniform distribution", "[plexdb.shard.token]") {
    constexpr U32 SHARD_COUNT = 8;
    constexpr U64 SAMPLES = 10000;

    U64 counts[SHARD_COUNT] = {};
    for (U64 i = 0; i < SAMPLES; i++) {
        U64 token = token_of(reinterpret_cast<const U8*>(&i), sizeof(i));
        U32 shard = owning_shard(token, SHARD_COUNT);
        REQUIRE(shard < SHARD_COUNT);
        counts[shard]++;
    }

    U64 expected = SAMPLES / SHARD_COUNT;
    for (U32 s = 0; s < SHARD_COUNT; s++) {
        REQUIRE(counts[s] > expected / 2);
        REQUIRE(counts[s] < expected * 2);
    }
}

TEST_CASE("owning_shard edge cases", "[plexdb.shard.token]") {
    SECTION("single shard") {
        REQUIRE(owning_shard(0, 1) == 0);
        REQUIRE(owning_shard(~0_u64, 1) == 0);
    }

    SECTION("boundary tokens") {
        REQUIRE(owning_shard(0, 4) == 0);
        U64 max_token = ~0_u64;
        U32 shard = owning_shard(max_token, 4);
        REQUIRE(shard == 3);
    }
}

TEST_CASE("shard_for_key combines hash and mapping", "[plexdb.shard.token]") {
    U8 key[] = "test_key";
    U32 shard = shard_for_key(key, sizeof(key) - 1, 16);
    REQUIRE(shard < 16);

    U32 shard2 = owning_shard(token_of(key, sizeof(key) - 1), 16);
    REQUIRE(shard == shard2);
}
