#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.column_types;
import cql.column_types.twos_complement;
import cql.column_types.vint;
import cql.engine.io.types;

using namespace plexdb;
using namespace cql;
using namespace cql::column_types;

namespace {
    VarInt make_varint(bool negative, const DynamicArray<U8>& magnitude_bytes) {
        VarInt v{};
        v.negative  = negative;
        v.magnitude = magnitude_bytes;
        return v;
    }

    DynamicArray<U8> bytes(std::initializer_list<U8> vals) {
        DynamicArray<U8> out;
        for (U8 b : vals) {
            push_back(out, b);
        }
        return out;
    }

    bool bytes_equal(const DynamicArray<U8>& a, std::initializer_list<U8> expected) {
        if (a.length != expected.size()) {
            return false;
        }
        U64 i = 0;
        for (U8 b : expected) {
            if (a.ptr[i++] != b) {
                return false;
            }
        }
        return true;
    }

    DynamicArray<U8> encode_varint(const VarInt& v) {
        DynamicArray<U8> out;
        auto             sink = io::sync_buffer_writer(out);
        twos_complement::write(io::to_writer(sink), v.negative, v.magnitude.ptr, v.magnitude.length);
        return out;
    }
}

TEST_CASE("twos_complement matches native protocol spec examples", "[column_types]") {
    // cassandra.apache.org/doc/latest/cassandra/reference/native-protocol.html §5.24
    CHECK(bytes_equal(encode_varint(make_varint(false, bytes({}))), {0x00}));
    CHECK(bytes_equal(encode_varint(make_varint(false, bytes({0x01}))), {0x01}));
    CHECK(bytes_equal(encode_varint(make_varint(false, bytes({0x7F}))), {0x7F}));
    CHECK(bytes_equal(encode_varint(make_varint(false, bytes({0x80}))), {0x00, 0x80}));
    CHECK(bytes_equal(encode_varint(make_varint(true, bytes({0x01}))), {0xFF}));
    CHECK(bytes_equal(encode_varint(make_varint(true, bytes({0x80}))), {0x80}));
    CHECK(bytes_equal(encode_varint(make_varint(true, bytes({0x81}))), {0xFF, 0x7F}));
}

TEST_CASE("twos_complement round-trips", "[column_types]") {
    DynamicArray<U8> magnitudes[] = {bytes({}), bytes({0x01}), bytes({0x7F}), bytes({0x80}), bytes({0x81}), bytes({0xFF, 0x00, 0x01})};
    for (bool negative : {false, true}) {
        for (const auto& mag : magnitudes) {
            VarInt                   in    = make_varint(negative, mag);
            DynamicArray<U8>         out   = encode_varint(in);
            twos_complement::Decoded round = twos_complement::read(out.ptr, out.length);
            VarInt                   rt    = make_varint(round.negative, round.magnitude);
            DynamicArray<U8>         out2  = encode_varint(rt);
            REQUIRE(out.length == out2.length);
            CHECK(os::memory_compare(out.ptr, out2.ptr, out.length) == 0);
        }
    }
}

TEST_CASE("vint zigzag round-trips", "[column_types]") {
    S64 values[] = {0, 1, -1, 63, 64, -64, -65, 8191, 8192, -8192, -8193, MAX_S64, MIN_S64, 256000, -256000};
    for (S64 v : values) {
        DynamicArray<U8> out;
        auto             sink = io::sync_buffer_writer(out);
        vint::write(io::to_writer(sink), v);
        CHECK(out.length == vint::encoded_length(v));
        const U8* p   = out.ptr;
        const U8* end = out.ptr + out.length;
        CHECK(vint::read(p, end) == v);
        CHECK(p == end);
    }
}

TEST_CASE("vint matches spec worked example", "[column_types]") {
    // doc's example encodes the unsigned value 256000 as 3 bytes, first byte 0b110_00011 =
    // 0xC3 — vint::write zigzags first, so feed it zigzag^-1(256000) = 128000 to land on the
    // same unsigned-vint bytes the doc shows.
    DynamicArray<U8> out;
    auto             sink = io::sync_buffer_writer(out);
    vint::write(io::to_writer(sink), S64(128000));
    REQUIRE(bytes_equal(out, {0xC3, 0xE8, 0x00}));
}
