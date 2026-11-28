#include <catch2/catch_test_macros.hpp>

import plexdb.base;

using namespace plexdb;

TEST_CASE("MapFixedSentinel keeps colliding keys reachable after removal", "[plexdb.containers]") {
    // hash(U64 x) = x + 1, so with 8 slots the keys 1, 9 and 17 share a probe chain.
    MapFixedSentinel<U64, U64, 8> map;
    insert(map, U64(1)) = 10;
    insert(map, U64(9)) = 90;
    insert(map, U64(17)) = 170;

    remove(map, U64(1));
    REQUIRE(find(map, U64(1)) == nullptr);
    REQUIRE(find(map, U64(9)) != nullptr);
    CHECK(*find(map, U64(9)) == 90);
    REQUIRE(find(map, U64(17)) != nullptr);
    CHECK(*find(map, U64(17)) == 170);

    remove(map, U64(9));
    REQUIRE(find(map, U64(17)) != nullptr);
    CHECK(*find(map, U64(17)) == 170);

    insert(map, U64(25)) = 250;
    CHECK(*find(map, U64(25)) == 250);
    CHECK(*find(map, U64(17)) == 170);
}

TEST_CASE("MapFixedSentinel lookup of a missing key in a map full of tombstones", "[plexdb.containers]") {
    MapFixedSentinel<U64, U64, 4> map;
    for (U64 round = 0; round < 3; round++) {
        for (U64 k = 1; k <= 4; k++) {
            insert(map, k + round * 4) = k;
        }
        for (U64 k = 1; k <= 4; k++) {
            remove(map, k + round * 4);
        }
    }
    CHECK(find(map, U64(100)) == nullptr);
    CHECK(!try_remove(map, U64(100)));

    U64 count = 0;
    for (auto& kv : map) {
        (void)kv;
        count++;
    }
    CHECK(count == 0);
}
