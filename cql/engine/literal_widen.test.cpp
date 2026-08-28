#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.dynamic.containers;

import cql.column_types;
import cql.engine.literal_widen;

using namespace plexdb;
using namespace cql;
using namespace cql::column_types;

TEST_CASE("varint_from_s64/s64_from_varint_truncating round-trip", "[literal_widen]") {
    S64 values[] = {0, 1, -1, 127, 128, -128, -129, 255, 256, 12345, -12345, MAX_S64, S64(MIN_S64 + 1)};
    for (S64 v : values) {
        CHECK(s64_from_varint_truncating(varint_from_s64(v)) == v);
    }
}

TEST_CASE("try_s64_from_varint narrows when possible, empty when not", "[literal_widen]") {
    CHECK(try_s64_from_varint(varint_from_s64(42)).has_value());
    CHECK(*try_s64_from_varint(varint_from_s64(42)) == 42);

    VarInt too_big{};
    too_big.negative = false;
    for (U8 b : {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) {
        push_back(too_big.magnitude, b);
    }
    CHECK_FALSE(try_s64_from_varint(too_big).has_value());
}

TEST_CASE("decimal_from_f64 produces the shortest exact decimal, not binary noise", "[literal_widen]") {
    Decimal d = decimal_from_f64(19.99);
    CHECK(d.scale == 2);
    CHECK(s64_from_varint_truncating(d.unscaled) == 1999);
}

TEST_CASE("inet_from_string parses IPv4 and IPv6", "[literal_widen]") {
    auto v4 = inet_from_string("127.0.0.1");
    REQUIRE(v4.has_value());
    CHECK_FALSE(v4->is_v6);
    CHECK(v4->v4[0] == 127);
    CHECK(v4->v4[3] == 1);

    auto v6 = inet_from_string("2001:db8::1");
    REQUIRE(v6.has_value());
    CHECK(v6->is_v6);
    CHECK(v6->v6[0] == 0x20);
    CHECK(v6->v6[1] == 0x01);
    CHECK(v6->v6[14] == 0x00);
    CHECK(v6->v6[15] == 0x01);

    CHECK_FALSE(inet_from_string("not-an-address").has_value());
    CHECK_FALSE(inet_from_string("256.0.0.1").has_value());
}
