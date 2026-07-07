#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.dynamic.containers;

import cql.engine.column_value;

using namespace plexdb;
using namespace cql;

// @note DynamicSet/DynamicMap (core/dynamic/containers.cppm) are hash-bucketed with
// load-triggered rehashing, so two instances holding identical elements can have different
// slots.length and therefore iterate in different orders. NestedColumnValue equality/hash for the
// Set/Map arms must not depend on that iteration order.
TEST_CASE("NestedColumnValue set equality and hash are independent of bucket layout", "[cql.engine.column_value]") {
    DynamicSet<NestedColumnValue> a{};
    insert(a, NestedColumnValue{ColumnValue{S64(1)}});
    insert(a, NestedColumnValue{ColumnValue{S64(2)}});
    insert(a, NestedColumnValue{ColumnValue{S64(3)}});

    DynamicSet<NestedColumnValue> b{};
    insert(b, NestedColumnValue{ColumnValue{S64(3)}});
    insert(b, NestedColumnValue{ColumnValue{S64(1)}});
    insert(b, NestedColumnValue{ColumnValue{S64(2)}});
    rehash(b, 17_u64); // force a different bucket layout than a's default slot count

    NestedColumnValue va{ColumnValue{move(a)}};
    NestedColumnValue vb{ColumnValue{move(b)}};

    REQUIRE(va == vb);
    REQUIRE(hash(va) == hash(vb));
}

TEST_CASE("NestedColumnValue map equality and hash are independent of bucket layout", "[cql.engine.column_value]") {
    DynamicMap<NestedColumnValue, NestedColumnValue> a{};
    insert(a, NestedColumnValue{ColumnValue{AutoString8("k1")}}, NestedColumnValue{ColumnValue{S64(1)}});
    insert(a, NestedColumnValue{ColumnValue{AutoString8("k2")}}, NestedColumnValue{ColumnValue{S64(2)}});

    DynamicMap<NestedColumnValue, NestedColumnValue> b{};
    insert(b, NestedColumnValue{ColumnValue{AutoString8("k2")}}, NestedColumnValue{ColumnValue{S64(2)}});
    insert(b, NestedColumnValue{ColumnValue{AutoString8("k1")}}, NestedColumnValue{ColumnValue{S64(1)}});
    rehash(b, 23_u64);

    NestedColumnValue va{ColumnValue{move(a)}};
    NestedColumnValue vb{ColumnValue{move(b)}};

    REQUIRE(va == vb);
    REQUIRE(hash(va) == hash(vb));
}

TEST_CASE("NestedColumnValue set inequality still detected across bucket layouts", "[cql.engine.column_value]") {
    DynamicSet<NestedColumnValue> a{};
    insert(a, NestedColumnValue{ColumnValue{S64(1)}});
    insert(a, NestedColumnValue{ColumnValue{S64(2)}});

    DynamicSet<NestedColumnValue> b{};
    insert(b, NestedColumnValue{ColumnValue{S64(1)}});
    insert(b, NestedColumnValue{ColumnValue{S64(3)}});
    rehash(b, 17_u64);

    NestedColumnValue va{ColumnValue{move(a)}};
    NestedColumnValue vb{ColumnValue{move(b)}};

    REQUIRE_FALSE(va == vb);
}

TEST_CASE("NestedColumnValue float hash agrees with equality for signed zero", "[cql.engine.column_value]") {
    NestedColumnValue pos_zero{ColumnValue{F64(0.0)}};
    NestedColumnValue neg_zero{ColumnValue{F64(-0.0)}};

    REQUIRE(pos_zero == neg_zero);
    REQUIRE(hash(pos_zero) == hash(neg_zero));

    NestedColumnValue pos_zero_f32{ColumnValue{F32(0.0f)}};
    NestedColumnValue neg_zero_f32{ColumnValue{F32(-0.0f)}};

    REQUIRE(pos_zero_f32 == neg_zero_f32);
    REQUIRE(hash(pos_zero_f32) == hash(neg_zero_f32));
}
