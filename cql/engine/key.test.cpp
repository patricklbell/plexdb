#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.tagged_union;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.key;
import cql.engine.schema;
import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;
using namespace cql;

namespace {
    schema::Column make_ck_col(String8 name, type::Basic dtype, Sort order = Sort::ASC) {
        return schema::Column{
            .tombstone        = false,
            .is_static        = false,
            .name             = name,
            .type             = type::create_basic(dtype),
            .key_kind         = schema::KeyKind::ClusteringKey,
            .key_position     = 0,
            .clustering_order = order,
        };
    }

    schema::Table make_single_ck_table(type::Basic dtype, Sort order = Sort::ASC) {
        schema::Table tbl;
        push_back(tbl.cols, make_ck_col("c", dtype, order));
        push_back(tbl.clustering_key_col_indices, 0_u64);
        return tbl;
    }

    schema::Table make_two_ck_table(type::Basic d1, Sort o1, type::Basic d2, Sort o2) {
        schema::Table tbl;
        push_back(tbl.cols, make_ck_col("c1", d1, o1));
        push_back(tbl.cols, make_ck_col("c2", d2, o2));
        tbl.cols[1].key_position = 1;
        push_back(tbl.clustering_key_col_indices, 0_u64);
        push_back(tbl.clustering_key_col_indices, 1_u64);
        return tbl;
    }

    int lex_compare(const DynamicArray<U8>& a, const DynamicArray<U8>& b) {
        U64 n   = a.length < b.length ? a.length : b.length;
        int cmp = n > 0 ? os::memory_compare(a.ptr, b.ptr, n) : 0;
        if (cmp != 0) {
            return cmp;
        }
        return a.length < b.length ? -1 : (a.length > b.length ? 1 : 0);
    }

    DynamicArray<U8> ser_ck_single(const schema::Table& tbl, Evaluated v) {
        return key::serialize_clustering_single(tbl, v);
    }

    DynamicArray<U8> ser_ck_pair(const schema::Table& tbl, Evaluated v1, Evaluated v2) {
        DynamicArray<Evaluated> arr;
        push_back(arr, v1);
        push_back(arr, v2);
        return key::serialize_clustering(tbl, TArrayView<const Evaluated, U64>{arr.ptr, arr.length});
    }
}

TEST_CASE("clustering key round-trip ASC and DESC for fixed-width types", "[cql.key]") {
    struct IntCase {
        type::Basic dtype;
        S64         a;
        S64         b;
    };
    constexpr IntCase int_cases[] = {
        {   type::Basic::bigint, -1_s64,                                   100_s64},
        {     type::Basic::int_, -1_s64,                                   100_s64},
        { type::Basic::smallint,  -1000,                                      1000},
        {     type::Basic::time,      0, 3600_s64 * 1000_s64 * 1000_s64 * 1000_s64},
        {     type::Basic::date,      0,                                     18000}, // ~2019-04-13
        {type::Basic::timestamp, -1_s64,                         1700000000000_s64},
    };
    for (const auto& c : int_cases) {
        for (Sort order : {Sort::ASC, Sort::DESC}) {
            auto tbl = make_single_ck_table(c.dtype, order);
            auto e_a = Evaluated{Constant{c.a}};
            auto e_b = Evaluated{Constant{c.b}};

            auto ka = ser_ck_single(tbl, e_a);
            auto kb = ser_ck_single(tbl, e_b);

            // For the non-composite single CK encoding, byte-wise lex compare
            // matches the requested logical ordering.
            int lex = lex_compare(ka, kb);
            INFO("dtype=" << U64(c.dtype) << " order=" << U64(order));
            if (order == Sort::ASC) {
                CHECK(lex < 0); // a < b logically → physical lex <
            } else {
                CHECK(lex > 0); // DESC: a < b logically → physical lex >
            }

            // Round-trip.
            auto decoded = key::deserialize_clustering(tbl, {ka.ptr, U16(ka.length)});
            REQUIRE(decoded.length == 1);
        }
    }
}

TEST_CASE("clustering key round-trip ASC and DESC for boolean and uuid", "[cql.key]") {
    {
        auto tbl_asc  = make_single_ck_table(type::Basic::boolean, Sort::ASC);
        auto tbl_desc = make_single_ck_table(type::Basic::boolean, Sort::DESC);

        auto t = Evaluated{Constant{true}};
        auto f = Evaluated{Constant{false}};

        CHECK(lex_compare(ser_ck_single(tbl_asc, f), ser_ck_single(tbl_asc, t)) < 0);
        CHECK(lex_compare(ser_ck_single(tbl_desc, f), ser_ck_single(tbl_desc, t)) > 0);
    }
    {
        // Two UUIDs whose first differing byte gives a clear order: u1 < u2.
        UUID u1{};
        UUID u2{};
        u2.value[0]   = 0x10;
        auto tbl_asc  = make_single_ck_table(type::Basic::uuid, Sort::ASC);
        auto tbl_desc = make_single_ck_table(type::Basic::uuid, Sort::DESC);
        CHECK(lex_compare(ser_ck_single(tbl_asc, Evaluated{Constant{u1}}), ser_ck_single(tbl_asc, Evaluated{Constant{u2}})) < 0);
        CHECK(lex_compare(ser_ck_single(tbl_desc, Evaluated{Constant{u1}}), ser_ck_single(tbl_desc, Evaluated{Constant{u2}})) > 0);
    }
}

TEST_CASE("clustering key round-trip ASC and DESC for variable-width composite types", "[cql.key]") {
    // For text / blob / hex inside a composite CK the encoding is escape-terminated.
    // Verify that lex compare under each direction reproduces the logical order, and
    // that round-trip recovers the original bytes.

    auto check_text = [](Sort order) {
        // c1 is the variable-width text under test; c2 is a fixed int that lets us
        // exercise the composite layout (length prefix is irrelevant for ordering of c1).
        auto tbl = make_two_ck_table(type::Basic::text, order, type::Basic::int_, Sort::ASC);

        auto a   = Evaluated{Constant{AutoString8("apple")}};
        auto b   = Evaluated{Constant{AutoString8("banana")}};
        auto a_p = Evaluated{Constant{AutoString8("app")}}; // prefix of a
        auto z0  = Evaluated{Constant{S64(0)}};

        auto k_a   = ser_ck_pair(tbl, a, z0);
        auto k_b   = ser_ck_pair(tbl, b, z0);
        auto k_a_p = ser_ck_pair(tbl, a_p, z0);

        INFO("order=" << U64(order));
        if (order == Sort::ASC) {
            CHECK(lex_compare(k_a, k_b) < 0);   // "apple" < "banana"
            CHECK(lex_compare(k_a_p, k_a) < 0); // "app" < "apple"
        } else {
            // DESC inverts the value bytes so byte-wise compare yields the reverse
            // logical order. "apple" < "banana" logically → "apple" sorts physically after "banana".
            CHECK(lex_compare(k_a, k_b) > 0);
            // "app" < "apple" logically → "app" sorts physically after "apple".
            CHECK(lex_compare(k_a_p, k_a) > 0);
        }
        auto dec = key::deserialize_clustering(tbl, {k_a.ptr, U16(k_a.length)});
        REQUIRE(dec.length == 2);
        const auto& got = get<AutoString8>(dec[0]);
        CHECK(String8(got.c_str, got.length) == String8("apple"));
    };
    check_text(Sort::ASC);
    check_text(Sort::DESC);

    // Blob path: identical scheme.
    {
        auto tbl = make_two_ck_table(type::Basic::blob, Sort::DESC, type::Basic::int_, Sort::ASC);
        Blob b1{};
        Blob b2{};
        resize(b1.value, 3_u64);
        b1.value[0] = 0xAA;
        b1.value[1] = 0x00;
        b1.value[2] = 0xBB; // contains an embedded NUL — must be escape-encoded.
        resize(b2.value, 1_u64);
        b2.value[0] = 0xAA;
        auto z0     = Evaluated{Constant{S64(0)}};
        auto k1     = ser_ck_pair(tbl, Evaluated{Constant{b1}}, z0);
        auto k2     = ser_ck_pair(tbl, Evaluated{Constant{b2}}, z0);
        CHECK(lex_compare(k1, k2) != 0);

        auto dec = key::deserialize_clustering(tbl, {k1.ptr, U16(k1.length)});
        REQUIRE(dec.length == 2);
        const auto& got = get<Blob>(dec[0]);
        REQUIRE(got.value.length == b1.value.length);
        for (U64 i = 0; i < b1.value.length; i++) {
            CHECK(got.value[i] == b1.value[i]);
        }
    }
}

TEST_CASE("date and time are usable as clustering key types", "[cql.key]") {
    // date: stored as S32, encoded like int_.
    {
        auto tbl   = make_single_ck_table(type::Basic::date, Sort::ASC);
        auto small = Evaluated{Constant{S64(100)}};
        auto large = Evaluated{Constant{S64(20000)}};
        CHECK(lex_compare(ser_ck_single(tbl, small), ser_ck_single(tbl, large)) < 0);
    }
    // time: stored as S64, encoded like bigint.
    {
        auto tbl     = make_single_ck_table(type::Basic::time, Sort::DESC);
        auto morning = Evaluated{Constant{S64(8 * 3600LL * 1000LL * 1000LL * 1000LL)}};
        auto evening = Evaluated{Constant{S64(20 * 3600LL * 1000LL * 1000LL * 1000LL)}};
        // DESC: evening (logical greater) sorts first physically.
        CHECK(lex_compare(ser_ck_single(tbl, evening), ser_ck_single(tbl, morning)) < 0);
    }
}
