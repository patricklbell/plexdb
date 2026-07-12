#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

import cql.engine.clustering_compare;
import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.io.codec;
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
        push_back(tbl.clustering_key_specs, clustering_compare::ClusteringColumnSpec{dtype, order});
        return tbl;
    }

    schema::Table make_two_ck_table(type::Basic d1, Sort o1, type::Basic d2, Sort o2) {
        schema::Table tbl;
        push_back(tbl.cols, make_ck_col("c1", d1, o1));
        push_back(tbl.cols, make_ck_col("c2", d2, o2));
        tbl.cols[1].key_position = 1;
        push_back(tbl.clustering_key_col_indices, 0_u64);
        push_back(tbl.clustering_key_col_indices, 1_u64);
        push_back(tbl.clustering_key_specs, clustering_compare::ClusteringColumnSpec{d1, o1});
        push_back(tbl.clustering_key_specs, clustering_compare::ClusteringColumnSpec{d2, o2});
        return tbl;
    }

    // Test-local convenience matching key.cppm's private append_component: writes one
    // column value in the same self-delimiting wire shape a real clustering key uses, so
    // these comparator tests can build raw key bytes without going through Evaluated.
    void encode_component(DynamicArray<U8>& out, const ColumnValue& cv, type::Basic dtype) {
        auto       sink = io::sync_buffer_writer(out);
        io::Writer w    = io::to_writer(sink);
        io::write_column_value(w, cv, type::Type{dtype});
    }

    DynamicArray<U8> ser_ck_single(const schema::Table& tbl, Evaluated v) {
        return key::encode_clustering_single(tbl, v);
    }

    DynamicArray<U8> ser_ck_pair(const schema::Table& tbl, Evaluated v1, Evaluated v2) {
        DynamicArray<Evaluated> arr;
        push_back(arr, v1);
        push_back(arr, v2);
        return key::encode_clustering(tbl, TArrayView<const Evaluated, U64>{arr.ptr, arr.length});
    }

    Ordering ck_order(const schema::Table& tbl, const DynamicArray<U8>& a, const DynamicArray<U8>& b) {
        auto policy = schema::make_clustering_key_policy(tbl);
        return policy.comparator(
            TArrayView<const U8, U16>(a.ptr, U16(a.length)),
            TArrayView<const U8, U16>(b.ptr, U16(b.length))
        );
    }

    VarInt make_varint(bool negative, TArrayView<const U8, U64> magnitude) {
        VarInt v;
        v.negative = negative;
        resize(v.magnitude, magnitude.length);
        for (U64 i = 0; i < magnitude.length; i++) {
            v.magnitude[i] = magnitude[i];
        }
        return v;
    }

    // RFC4122 v1 fields: time_low | time_mid | (time_hi & 0x0FFF), version nibble forced to 1.
    UUID make_timeuuid(U32 time_low, U16 time_mid, U16 time_hi_12bits) {
        UUID u{};
        u.value[0]     = U8(time_low >> 24);
        u.value[1]     = U8(time_low >> 16);
        u.value[2]     = U8(time_low >> 8);
        u.value[3]     = U8(time_low);
        u.value[4]     = U8(time_mid >> 8);
        u.value[5]     = U8(time_mid);
        U16 hi_and_ver = U16((time_hi_12bits & 0x0FFF) | (1u << 12));
        u.value[6]     = U8(hi_and_ver >> 8);
        u.value[7]     = U8(hi_and_ver);
        return u;
    }

    UUID make_uuid_with_version(U8 version, U8 fill) {
        UUID u{};
        for (U64 i = 0; i < UUID::length; i++) {
            u.value[i] = fill;
        }
        u.value[6] = U8((version << 4) | (u.value[6] & 0x0F));
        return u;
    }

    schema::Table make_single_pk_table(type::Basic dtype) {
        schema::Table  tbl;
        schema::Column col{
            .tombstone    = false,
            .is_static    = false,
            .name         = "pk",
            .type         = type::create_basic(dtype),
            .key_kind     = schema::KeyKind::PartitionKey,
            .key_position = 0,
        };
        push_back(tbl.cols, col);
        push_back(tbl.partition_key_col_indices, 0_u64);
        return tbl;
    }
}

TEST_CASE("clustering key round-trip and comparator order for fixed-width types", "[cql.key]") {
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
            auto e_a = Evaluated{Literal{c.a}};
            auto e_b = Evaluated{Literal{c.b}};

            auto ka = ser_ck_single(tbl, e_a);
            auto kb = ser_ck_single(tbl, e_b);

            INFO("dtype=" << U64(c.dtype) << " order=" << U64(order));
            Ordering ord = ck_order(tbl, ka, kb);
            if (order == Sort::ASC) {
                CHECK(ord == Ordering::Less); // a < b logically
            } else {
                CHECK(ord == Ordering::Greater); // DESC reverses per-column
            }

            auto decoded = key::decode_clustering(tbl, {ka.ptr, U16(ka.length)});
            REQUIRE(decoded.length == 1);
        }
    }
}

TEST_CASE("clustering key comparator order for boolean and uuid", "[cql.key]") {
    {
        auto tbl_asc  = make_single_ck_table(type::Basic::boolean, Sort::ASC);
        auto tbl_desc = make_single_ck_table(type::Basic::boolean, Sort::DESC);

        auto t = Evaluated{Literal{true}};
        auto f = Evaluated{Literal{false}};

        CHECK(ck_order(tbl_asc, ser_ck_single(tbl_asc, f), ser_ck_single(tbl_asc, t)) == Ordering::Less);
        CHECK(ck_order(tbl_desc, ser_ck_single(tbl_desc, f), ser_ck_single(tbl_desc, t)) == Ordering::Greater);
    }
    {
        // Non-time-based (version != 1) UUIDs fall back to raw unsigned byte compare.
        UUID u1       = make_uuid_with_version(4, 0x00);
        UUID u2       = make_uuid_with_version(4, 0x00);
        u2.value[0]   = 0x10;
        auto tbl_asc  = make_single_ck_table(type::Basic::uuid, Sort::ASC);
        auto tbl_desc = make_single_ck_table(type::Basic::uuid, Sort::DESC);
        CHECK(ck_order(tbl_asc, ser_ck_single(tbl_asc, Evaluated{Literal{u1}}), ser_ck_single(tbl_asc, Evaluated{Literal{u2}})) == Ordering::Less);
        CHECK(ck_order(tbl_desc, ser_ck_single(tbl_desc, Evaluated{Literal{u1}}), ser_ck_single(tbl_desc, Evaluated{Literal{u2}})) == Ordering::Greater);
    }
    {
        // Version nibble dominates: a version-1 UUID sorts before a version-4 UUID
        // regardless of trailing byte content.
        UUID v1  = make_uuid_with_version(1, 0xFF);
        UUID v4  = make_uuid_with_version(4, 0x00);
        auto tbl = make_single_ck_table(type::Basic::uuid, Sort::ASC);
        CHECK(ck_order(tbl, ser_ck_single(tbl, Evaluated{Literal{v1}}), ser_ck_single(tbl, Evaluated{Literal{v4}})) == Ordering::Less);
    }
    {
        // Round-trip still recovers the original bytes.
        UUID u   = make_uuid_with_version(4, 0x42);
        auto tbl = make_single_ck_table(type::Basic::uuid, Sort::ASC);
        auto k   = ser_ck_single(tbl, Evaluated{Literal{u}});
        auto dec = key::decode_clustering(tbl, {k.ptr, U16(k.length)});
        REQUIRE(dec.length == 1);
        CHECK(get<UUID>(dec[0]) == u);
    }
}

TEST_CASE("clustering key comparator orders timeuuid by embedded timestamp, not raw bytes", "[cql.key]") {
    // A's timestamp is smaller than B's, but A's raw bytes (time_low dominates the
    // leading bytes) are larger than B's — proves ordering comes from the
    // reassembled timestamp, not a byte-lexicographic compare.
    UUID a = make_timeuuid(0xFFFFFFFF, 0, 0x000); // timestamp = 0xFFFFFFFF
    UUID b = make_timeuuid(0x00000000, 0, 0x001); // timestamp = 1<<48, much larger

    REQUIRE(a.value[0] > b.value[0]); // raw-byte order would say a > b

    auto tbl = make_single_ck_table(type::Basic::timeuuid, Sort::ASC);
    CHECK(ck_order(tbl, ser_ck_single(tbl, Evaluated{Literal{a}}), ser_ck_single(tbl, Evaluated{Literal{b}})) == Ordering::Less);

    auto tbl_desc = make_single_ck_table(type::Basic::timeuuid, Sort::DESC);
    CHECK(ck_order(tbl_desc, ser_ck_single(tbl_desc, Evaluated{Literal{a}}), ser_ck_single(tbl_desc, Evaluated{Literal{b}})) == Ordering::Greater);

    auto k   = ser_ck_single(tbl, Evaluated{Literal{a}});
    auto dec = key::decode_clustering(tbl, {k.ptr, U16(k.length)});
    REQUIRE(dec.length == 1);
    CHECK(get<UUID>(dec[0]) == a);
}

TEST_CASE("clustering key comparator order for decimal and varint", "[cql.key]") {
    {
        U8   mag_small[] = {0x01};
        U8   mag_large[] = {0x64}; // 100
        auto tbl         = make_single_ck_table(type::Basic::varint, Sort::ASC);

        auto             small = ColumnValue{make_varint(false, mag_small)};
        auto             large = ColumnValue{make_varint(false, mag_large)};
        DynamicArray<U8> ka, kb;
        encode_component(ka, small, type::Basic::varint);
        encode_component(kb, large, type::Basic::varint);
        CHECK(ck_order(tbl, ka, kb) == Ordering::Less);

        auto             negative = ColumnValue{make_varint(true, mag_large)};
        DynamicArray<U8> kn;
        encode_component(kn, negative, type::Basic::varint);
        CHECK(ck_order(tbl, kn, ka) == Ordering::Less); // -100 < 1

        auto dec = key::decode_clustering(tbl, {ka.ptr, U16(ka.length)});
        REQUIRE(dec.length == 1);
        CHECK(get<VarInt>(dec[0]) == get<VarInt>(small));
    }
    {
        // 1.25 (scale=2, unscaled=125) vs 1 (scale=0, unscaled=1): different scale,
        // must compare numerically after scale-alignment, not by magnitude length.
        auto tbl = make_single_ck_table(type::Basic::decimal, Sort::ASC);

        U8      mag_125[] = {125};
        U8      mag_1[]   = {1};
        Decimal one_quarter{.scale = 2, .unscaled = make_varint(false, mag_125)};
        Decimal one{.scale = 0, .unscaled = make_varint(false, mag_1)};

        DynamicArray<U8> ka, kb;
        encode_component(ka, ColumnValue{one_quarter}, type::Basic::decimal);
        encode_component(kb, ColumnValue{one}, type::Basic::decimal);
        CHECK(ck_order(tbl, kb, ka) == Ordering::Less); // 1 < 1.25

        // Same numeric value at different scales compares Equal.
        U8               mag_10[] = {10};
        Decimal          one_alt_scale{.scale = 1, .unscaled = make_varint(false, mag_10)}; // 1.0
        DynamicArray<U8> kc;
        encode_component(kc, ColumnValue{one_alt_scale}, type::Basic::decimal);
        CHECK(ck_order(tbl, kb, kc) == Ordering::Equal);

        auto dec = key::decode_clustering(tbl, {ka.ptr, U16(ka.length)});
        REQUIRE(dec.length == 1);
        CHECK(get<Decimal>(dec[0]) == one_quarter);
    }
}

TEST_CASE("clustering key comparator order for inet", "[cql.key]") {
    Inet v4_a{};
    v4_a.is_v6 = false;
    v4_a.v4    = Array<U8, 4>{10, 0, 0, 1};
    Inet v4_b{};
    v4_b.is_v6 = false;
    v4_b.v4    = Array<U8, 4>{10, 0, 0, 2};
    Inet v6{};
    v6.is_v6 = true;
    v6.v6    = Array<U8, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

    auto tbl = make_single_ck_table(type::Basic::inet, Sort::ASC);

    DynamicArray<U8> ka, kb, k6;
    encode_component(ka, ColumnValue{v4_a}, type::Basic::inet);
    encode_component(kb, ColumnValue{v4_b}, type::Basic::inet);
    encode_component(k6, ColumnValue{v6}, type::Basic::inet);

    CHECK(ck_order(tbl, ka, kb) == Ordering::Less);  // 10.0.0.1 < 10.0.0.2
    CHECK(ck_order(tbl, ka, k6) != Ordering::Equal); // different length/family

    auto dec = key::decode_clustering(tbl, {ka.ptr, U16(ka.length)});
    REQUIRE(dec.length == 1);
    CHECK(get<Inet>(dec[0]) == v4_a);
}

TEST_CASE("clustering key round-trip ASC and DESC for variable-width composite types", "[cql.key]") {
    // For text / blob inside a composite CK the encoding is a U16 length prefix +
    // raw bytes; verify the comparator reproduces the logical order under both
    // directions, and that round-trip recovers the original bytes.

    auto check_text = [](Sort order) {
        // c1 is the variable-width text under test; c2 is a fixed int that lets us
        // exercise the composite layout.
        auto tbl = make_two_ck_table(type::Basic::text, order, type::Basic::int_, Sort::ASC);

        auto a   = Evaluated{Literal{AutoString8("apple")}};
        auto b   = Evaluated{Literal{AutoString8("banana")}};
        auto a_p = Evaluated{Literal{AutoString8("app")}}; // prefix of a
        auto z0  = Evaluated{Literal{S64(0)}};

        auto k_a   = ser_ck_pair(tbl, a, z0);
        auto k_b   = ser_ck_pair(tbl, b, z0);
        auto k_a_p = ser_ck_pair(tbl, a_p, z0);

        INFO("order=" << U64(order));
        if (order == Sort::ASC) {
            CHECK(ck_order(tbl, k_a, k_b) == Ordering::Less);   // "apple" < "banana"
            CHECK(ck_order(tbl, k_a_p, k_a) == Ordering::Less); // "app" < "apple"
        } else {
            CHECK(ck_order(tbl, k_a, k_b) == Ordering::Greater);
            CHECK(ck_order(tbl, k_a_p, k_a) == Ordering::Greater);
        }
        auto dec = key::decode_clustering(tbl, {k_a.ptr, U16(k_a.length)});
        REQUIRE(dec.length == 2);
        const auto& got = get<AutoString8>(dec[0]);
        CHECK(String8(got.c_str, got.length) == String8("apple"));
    };
    check_text(Sort::ASC);
    check_text(Sort::DESC);

    {
        auto tbl = make_two_ck_table(type::Basic::blob, Sort::DESC, type::Basic::int_, Sort::ASC);
        Blob b1{};
        Blob b2{};
        resize(b1.value, 3_u64);
        b1.value[0] = 0xAA;
        b1.value[1] = 0x00;
        b1.value[2] = 0xBB;
        resize(b2.value, 1_u64);
        b2.value[0] = 0xAA;
        auto z0     = Evaluated{Literal{S64(0)}};
        auto k1     = ser_ck_pair(tbl, Evaluated{Literal{b1}}, z0);
        auto k2     = ser_ck_pair(tbl, Evaluated{Literal{b2}}, z0);
        CHECK(ck_order(tbl, k1, k2) != Ordering::Equal);

        auto dec = key::decode_clustering(tbl, {k1.ptr, U16(k1.length)});
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
        auto small = Evaluated{Literal{S64(100)}};
        auto large = Evaluated{Literal{S64(20000)}};
        CHECK(ck_order(tbl, ser_ck_single(tbl, small), ser_ck_single(tbl, large)) == Ordering::Less);
    }
    // time: stored as S64, encoded like bigint.
    {
        auto tbl     = make_single_ck_table(type::Basic::time, Sort::DESC);
        auto morning = Evaluated{Literal{S64(8 * 3600LL * 1000LL * 1000LL * 1000LL)}};
        auto evening = Evaluated{Literal{S64(20 * 3600LL * 1000LL * 1000LL * 1000LL)}};
        // DESC: evening (logical greater) sorts first.
        CHECK(ck_order(tbl, ser_ck_single(tbl, evening), ser_ck_single(tbl, morning)) == Ordering::Less);
    }
}

// Regression coverage for a bug in the old per-arm partition-token encoder: tinyint,
// float, text, uuid, blob, and hex all read the Evaluated via get<Literal>(...) directly,
// which asserts (crashes) when handed a ColumnValue-arm Evaluated — the shape
// evaluator.cpp's lookup_column_value produces for a `token(some_column)` reference.
// compute_partition_token_from_evals must accept that shape for every partition-key type.
TEST_CASE("compute_partition_token_from_evals accepts a ColumnValue-arm Evaluated for every partition key type", "[cql.key]") {
    struct Case {
        type::Basic dtype;
        ColumnValue value;
    };
    Blob blob_val{};
    push_back(blob_val.value, U8(0xDE));
    push_back(blob_val.value, U8(0xAD));
    push_back(blob_val.value, U8(0xBE));
    push_back(blob_val.value, U8(0xEF));

    const Case cases[] = {
        {type::Basic::tinyint, ColumnValue{U8(static_cast<U8>(static_cast<S8>(-5)))}},
        { type::Basic::float_,                                ColumnValue{F32(3.5f)}},
        {   type::Basic::text,                     ColumnValue{AutoString8("hello")}},
        {   type::Basic::uuid, ColumnValue{make_uuid_with_version(4, /*fill=*/0x11)}},
        {   type::Basic::blob,                                 ColumnValue{blob_val}},
    };

    for (const auto& c : cases) {
        INFO("dtype=" << U64(c.dtype));
        auto tbl = make_single_pk_table(c.dtype);

        // The known-safe path: token computed directly from ColumnValue.
        S64 expected = key::compute_partition_token(tbl, {&c.value, 1});

        // The previously-crashing path: a ColumnValue wrapped as an Evaluated, matching
        // what a `token(column_ref)` term evaluates to.
        Evaluated               col_ref_eval{c.value};
        DynamicArray<Evaluated> evals;
        push_back(evals, col_ref_eval);
        S64 from_columnvalue_eval = key::compute_partition_token_from_evals(tbl, {evals.ptr, evals.length});

        CHECK(from_columnvalue_eval == expected);
    }
}
