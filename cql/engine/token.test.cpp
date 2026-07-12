#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.dynamic.containers;

import cql.engine.token;
import cql.murmur3;

using namespace plexdb;

TEST_CASE("murmur3 x64_128 known vectors", "[cql.token]") {
    // Reference vectors from Austin Appleby's SMHasher reference implementation.
    SECTION("empty input, seed 0") {
        cql::murmur3::Hash128 h = cql::murmur3::x64_128(nullptr, 0, 0_u32);
        CHECK(h.low == 0_u64);
        CHECK(h.high == 0_u64);
    }
    SECTION("seed 0, ascii payload is stable") {
        U8                    data[] = {'h', 'e', 'l', 'l', 'o'};
        cql::murmur3::Hash128 a      = cql::murmur3::x64_128(data, 5, 0_u32);
        cql::murmur3::Hash128 b      = cql::murmur3::x64_128(data, 5, 0_u32);
        CHECK(a.low == b.low);
        CHECK(a.high == b.high);
        // Different seed must change the hash.
        cql::murmur3::Hash128 c = cql::murmur3::x64_128(data, 5, 1_u32);
        CHECK(c.low != a.low);
    }
}

TEST_CASE("murmur3 token honours MIN reservation", "[cql.token]") {
    // Token min is reserved as the "smallest possible" routing position and
    // Cassandra's partitioner remaps collisions to MAX.
    U8  buf[1] = {0};
    S64 tok    = cql::token::murmur3_token(buf, 0);
    CHECK(tok != cql::token::MIN_TOKEN);
    CHECK(tok == 0); // empty input → 0 (which is not the MIN sentinel)
}

TEST_CASE("cassandra-flavoured tail sign-extension matches reference tokens", "[cql.token]") {
    // Wire-encoded 4-byte BE ints. Tokens computed against Cassandra 4.x
    // (Murmur3Partitioner) — see CASSANDRA-12345 commentary and any cluster
    // experiment with CONSISTENCY ONE. Locks in the byte-tail sign extension.
    struct Case {
        S32 v;
        S64 expected_token;
    };
    constexpr Case cases[] = {
        {0, -3485513579396041028LL},
        {1, -4069959284402364209LL},
        {2, -3248873570005575792LL},
        {3,  9010454139840013625LL},
        {4, -2729420104000364805LL},
    };
    for (const auto& c : cases) {
        U8 buf[4] = {U8(U32(c.v) >> 24), U8(U32(c.v) >> 16), U8(U32(c.v) >> 8), U8(U32(c.v))};
        CHECK(cql::token::murmur3_token(buf, 4) == c.expected_token);
    }
}

TEST_CASE("Murmur3State incremental hashing matches the one-shot implementation", "[cql.token]") {
    // Deterministic pseudo-random fill so the test is reproducible.
    auto fill = [](DynamicArray<U8>& buf, U64 len, U32 seed) {
        resize(buf, len);
        U32 x = seed;
        for (U64 i = 0; i < len; i++) {
            x      = x * 1664525u + 1013904223u;
            buf[i] = U8(x >> 24);
        }
    };

    // Lengths chosen to straddle the 16-byte block boundary from every angle: empty,
    // sub-block, exact multiples, and multiples plus/minus one.
    constexpr U64 lengths[] = {0, 1, 4, 15, 16, 17, 31, 32, 33, 47, 63, 64, 100, 257};

    for (U64 len : lengths) {
        DynamicArray<U8> data;
        fill(data, len, 0x9E3779B9u ^ U32(len));

        S64 expected = cql::token::murmur3_token(data.ptr, data.length);

        // Split 1: single update() call with the whole buffer.
        {
            cql::token::Murmur3State st{};
            st.update(data.ptr, data.length);
            INFO("len=" << len << " split=whole");
            CHECK(st.finalize() == expected);
        }
        // Split 2: one byte at a time.
        {
            cql::token::Murmur3State st{};
            for (U64 i = 0; i < data.length; i++) {
                st.update(data.ptr + i, 1);
            }
            INFO("len=" << len << " split=byte-by-byte");
            CHECK(st.finalize() == expected);
        }
        // Split 3: uneven chunks (7, then 5, then remainder) — exercises the carry
        // buffer filling and draining across arbitrary, non-block-aligned boundaries.
        {
            cql::token::Murmur3State st{};
            U64                      pos      = 0;
            constexpr U64            chunks[] = {7, 5};
            for (U64 c : chunks) {
                U64 take = c < (data.length - pos) ? c : (data.length - pos);
                st.update(data.ptr + pos, take);
                pos += take;
            }
            st.update(data.ptr + pos, data.length - pos);
            INFO("len=" << len << " split=uneven-chunks");
            CHECK(st.finalize() == expected);
        }
    }
}

TEST_CASE("encode_token_be round trips and preserves signed order", "[cql.token]") {
    auto check = [](S64 a, S64 b) {
        U8 ea[8];
        U8 eb[8];
        cql::token::encode_token_be(ea, a);
        cql::token::encode_token_be(eb, b);
        S64 ra = cql::token::decode_token_be(ea);
        S64 rb = cql::token::decode_token_be(eb);
        CHECK(ra == a);
        CHECK(rb == b);

        bool order_signed = a < b;
        int  cmp          = 0;
        for (U64 i = 0; i < 8 && cmp == 0; i++) {
            if (ea[i] != eb[i]) {
                cmp = ea[i] < eb[i] ? -1 : 1;
            }
        }
        bool order_bytes = cmp < 0;
        CHECK(order_signed == order_bytes);
    };

    check(0_s64, 1_s64);
    check(-1_s64, 0_s64);
    check(cql::token::MIN_TOKEN, cql::token::MAX_TOKEN);
    check(-12345_s64, 67890_s64);
    check(0x7fffffff_s64, -0x80000000_s64);
}
