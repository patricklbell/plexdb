#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.types;
import cql.engine.statements;
import cql.engine.io;

using namespace plexdb;
using namespace cql;
using namespace cql::io;

struct Buffer {
    DynamicArray<U8> data{};
    U64 cursor = 0;

    auto writer() {
        return [this](const U8* src, U64 size) {
            for (U64 i = 0; i < size; i++) push_back(data, src[i]);
        };
    }

    auto reader() {
        return [this](U8* dst, U64 size) {
            for (U64 i = 0; i < size; i++) dst[i] = data[cursor + i];
            cursor += size;
        };
    }
};

TEST_CASE("io roundtrip - scalar types", "[cql.engine.io]") {
    SECTION("text") {
        Buffer buf;
        ColumnValue in{AutoString8("hello world")};
        write_column_value(buf.writer(), in, create_basic(BasicType::text));
        auto out = read_column_value(buf.reader(), BasicType::text);
        REQUIRE(type_matches_tag<AutoString8>(out));
        REQUIRE(get<AutoString8>(out) == "hello world");
    }

    SECTION("empty text") {
        Buffer buf;
        ColumnValue in{AutoString8("")};
        write_column_value(buf.writer(), in, create_basic(BasicType::text));
        auto out = read_column_value(buf.reader(), BasicType::text);
        REQUIRE(type_matches_tag<AutoString8>(out));
        REQUIRE(get<AutoString8>(out).length == 0);
    }

    SECTION("int") {
        Buffer buf;
        ColumnValue in{S32(-42)};
        write_column_value(buf.writer(), in, create_basic(BasicType::int_));
        auto out = read_column_value(buf.reader(), BasicType::int_);
        REQUIRE(type_matches_tag<S32>(out));
        REQUIRE(get<S32>(out) == -42);
    }

    SECTION("bigint") {
        Buffer buf;
        ColumnValue in{S64(9999999999LL)};
        write_column_value(buf.writer(), in, create_basic(BasicType::bigint));
        auto out = read_column_value(buf.reader(), BasicType::bigint);
        REQUIRE(type_matches_tag<S64>(out));
        REQUIRE(get<S64>(out) == 9999999999LL);
    }

    SECTION("smallint") {
        Buffer buf;
        ColumnValue in{S16(-32000)};
        write_column_value(buf.writer(), in, create_basic(BasicType::smallint));
        auto out = read_column_value(buf.reader(), BasicType::smallint);
        REQUIRE(type_matches_tag<S16>(out));
        REQUIRE(get<S16>(out) == -32000);
    }

    SECTION("boolean") {
        Buffer buf;
        ColumnValue in_t{U8(1)};
        write_column_value(buf.writer(), in_t, create_basic(BasicType::boolean));
        auto out_t = read_column_value(buf.reader(), BasicType::boolean);
        REQUIRE(type_matches_tag<U8>(out_t));
        REQUIRE(get<U8>(out_t) == 1);

        Buffer buf2;
        ColumnValue in_f{U8(0)};
        write_column_value(buf2.writer(), in_f, create_basic(BasicType::boolean));
        auto out_f = read_column_value(buf2.reader(), BasicType::boolean);
        REQUIRE(get<U8>(out_f) == 0);
    }

    SECTION("float") {
        Buffer buf;
        ColumnValue in{F32(3.14f)};
        write_column_value(buf.writer(), in, create_basic(BasicType::float_));
        auto out = read_column_value(buf.reader(), BasicType::float_);
        REQUIRE(type_matches_tag<F32>(out));
        REQUIRE(get<F32>(out) == 3.14f);
    }

    SECTION("double") {
        Buffer buf;
        ColumnValue in{F64(-2.71828)};
        write_column_value(buf.writer(), in, create_basic(BasicType::double_));
        auto out = read_column_value(buf.reader(), BasicType::double_);
        REQUIRE(type_matches_tag<F64>(out));
        REQUIRE(get<F64>(out) == -2.71828);
    }

    SECTION("uuid") {
        Buffer buf;
        UUID id;
        for (U64 i = 0; i < 16; i++) id.value[i] = static_cast<U8>(i + 1);
        ColumnValue in{id};
        write_column_value(buf.writer(), in, create_basic(BasicType::uuid));
        auto out = read_column_value(buf.reader(), BasicType::uuid);
        REQUIRE(type_matches_tag<UUID>(out));
        REQUIRE(get<UUID>(out) == id);
    }
}

TEST_CASE("io roundtrip - inet", "[cql.engine.io]") {
    SECTION("ipv4") {
        Buffer buf;
        Inet addr;
        addr.is_v6 = false;
        addr.v4[0] = 192; addr.v4[1] = 168; addr.v4[2] = 1; addr.v4[3] = 42;
        ColumnValue in{addr};
        write_column_value(buf.writer(), in, create_basic(BasicType::inet));
        auto out = read_column_value(buf.reader(), BasicType::inet);
        REQUIRE(type_matches_tag<Inet>(out));
        REQUIRE(get<Inet>(out) == addr);
    }

    SECTION("ipv6") {
        Buffer buf;
        Inet addr;
        addr.is_v6 = true;
        for (int i = 0; i < 16; i++) addr.v6[i] = static_cast<U8>(i * 0x11);
        ColumnValue in{addr};
        write_column_value(buf.writer(), in, create_basic(BasicType::inet));
        auto out = read_column_value(buf.reader(), BasicType::inet);
        REQUIRE(type_matches_tag<Inet>(out));
        REQUIRE(get<Inet>(out) == addr);
    }
}

TEST_CASE("io roundtrip - varint", "[cql.engine.io]") {
    SECTION("positive") {
        Buffer buf;
        VarInt val;
        val.negative = false;
        push_back(val.magnitude, U8(0x01));
        push_back(val.magnitude, U8(0x00));
        ColumnValue in{val};
        write_column_value(buf.writer(), in, create_basic(BasicType::varint));
        auto out = read_column_value(buf.reader(), BasicType::varint);
        REQUIRE(type_matches_tag<VarInt>(out));
        REQUIRE(get<VarInt>(out) == val);
    }

    SECTION("negative") {
        Buffer buf;
        VarInt val;
        val.negative = true;
        push_back(val.magnitude, U8(0xFF));
        ColumnValue in{val};
        write_column_value(buf.writer(), in, create_basic(BasicType::varint));
        auto out = read_column_value(buf.reader(), BasicType::varint);
        REQUIRE(type_matches_tag<VarInt>(out));
        REQUIRE(get<VarInt>(out) == val);
    }

    SECTION("zero") {
        Buffer buf;
        VarInt val;
        val.negative = false;
        ColumnValue in{val};
        write_column_value(buf.writer(), in, create_basic(BasicType::varint));
        auto out = read_column_value(buf.reader(), BasicType::varint);
        REQUIRE(type_matches_tag<VarInt>(out));
        REQUIRE(get<VarInt>(out).magnitude.length == 0);
    }
}

TEST_CASE("io roundtrip - decimal", "[cql.engine.io]") {
    Buffer buf;
    Decimal val;
    val.scale = 3;
    val.unscaled.negative = false;
    push_back(val.unscaled.magnitude, U8(0x27));
    push_back(val.unscaled.magnitude, U8(0x0F));
    ColumnValue in{val};
    write_column_value(buf.writer(), in, create_basic(BasicType::decimal));
    auto out = read_column_value(buf.reader(), BasicType::decimal);
    REQUIRE(type_matches_tag<Decimal>(out));
    REQUIRE(get<Decimal>(out) == val);
}

TEST_CASE("io roundtrip - duration", "[cql.engine.io]") {
    Buffer buf;
    Duration val{.months = 1, .days = 15, .nanoseconds = 3600000000000LL};
    ColumnValue in{val};
    write_column_value(buf.writer(), in, create_basic(BasicType::duration));
    auto out = read_column_value(buf.reader(), BasicType::duration);
    REQUIRE(type_matches_tag<Duration>(out));
    REQUIRE(get<Duration>(out) == val);
}

TEST_CASE("io roundtrip - collections", "[cql.engine.io]") {
    SECTION("list<text>") {
        Buffer buf;
        Type t = create_list(BasicType::text);
        DynamicArray<AutoString8> arr{};
        push_back(arr, AutoString8("alpha"));
        push_back(arr, AutoString8("beta"));
        push_back(arr, AutoString8("gamma"));
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<AutoString8>>(out));
        auto& got = get<DynamicArray<AutoString8>>(out);
        REQUIRE(got.length == 3);
        REQUIRE(got[0] == "alpha");
        REQUIRE(got[1] == "beta");
        REQUIRE(got[2] == "gamma");
    }

    SECTION("list<int>") {
        Buffer buf;
        Type t = create_list(BasicType::int_);
        DynamicArray<S32> arr{};
        push_back(arr, S32(1));
        push_back(arr, S32(-99));
        push_back(arr, S32(0));
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<S32>>(out));
        auto& got = get<DynamicArray<S32>>(out);
        REQUIRE(got.length == 3);
        REQUIRE(got[0] == 1);
        REQUIRE(got[1] == -99);
        REQUIRE(got[2] == 0);
    }

    SECTION("empty list") {
        Buffer buf;
        Type t = create_list(BasicType::bigint);
        DynamicArray<S64> arr{};
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<S64>>(out));
        REQUIRE(get<DynamicArray<S64>>(out).length == 0);
    }

    SECTION("vector<float>") {
        Buffer buf;
        Type t = create_vector(BasicType::float_, 3);
        DynamicArray<F32> arr{};
        push_back(arr, F32(1.0f));
        push_back(arr, F32(2.5f));
        push_back(arr, F32(-0.5f));
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<F32>>(out));
        auto& got = get<DynamicArray<F32>>(out);
        REQUIRE(got.length == 3);
        REQUIRE(got[0] == 1.0f);
        REQUIRE(got[1] == 2.5f);
        REQUIRE(got[2] == -0.5f);
    }

    SECTION("set<bigint>") {
        Buffer buf;
        Type t = create_set(BasicType::bigint);
        DynamicSet<S64> s{};
        insert(s, S64(100LL));
        insert(s, S64(200LL));
        insert(s, S64(300LL));
        ColumnValue in{move(s)};
        write_column_value(buf.writer(), in, t);
        auto out = read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicSet<S64>>(out));
        REQUIRE(length(get<DynamicSet<S64>>(out)) == 3);
    }

    SECTION("map<text,bigint>") {
        Buffer buf;
        Type t = create_map(BasicType::text, BasicType::bigint);
        DynamicMap<AutoString8, S64> m{};
        insert(m, AutoString8("key1"), S64(42LL));
        insert(m, AutoString8("key2"), S64(-1LL));
        ColumnValue in{move(m)};
        write_column_value(buf.writer(), in, t);
        auto out = read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicMap<AutoString8, S64>>(out));
        REQUIRE(length(get<DynamicMap<AutoString8, S64>>(out)) == 2);
    }
}

TEST_CASE("io write_default_column_value", "[cql.engine.io]") {
    auto check_default = [](BasicType dtype) {
        Buffer buf;
        write_default_column_value(buf.writer(), dtype);
        read_column_value(buf.reader(), dtype);
        REQUIRE(buf.cursor == buf.data.length);
    };

    check_default(BasicType::text);
    check_default(BasicType::int_);
    check_default(BasicType::bigint);
    check_default(BasicType::smallint);
    check_default(BasicType::boolean);
    check_default(BasicType::float_);
    check_default(BasicType::double_);
    check_default(BasicType::uuid);
    check_default(BasicType::blob);
    check_default(BasicType::inet);
    check_default(BasicType::varint);
    check_default(BasicType::decimal);
    check_default(BasicType::duration);

    SECTION("default int is zero") {
        Buffer buf;
        write_default_column_value(buf.writer(), BasicType::int_);
        auto out = read_column_value(buf.reader(), BasicType::int_);
        REQUIRE(get<S32>(out) == 0);
    }

    SECTION("default inet is ipv4 zero") {
        Buffer buf;
        write_default_column_value(buf.writer(), BasicType::inet);
        auto out = read_column_value(buf.reader(), BasicType::inet);
        REQUIRE(type_matches_tag<Inet>(out));
        REQUIRE(get<Inet>(out).is_v6 == false);
    }
}

TEST_CASE("io to_str - scalars", "[cql.engine.io]") {
    REQUIRE(to_str(ColumnValue{AutoString8("hello")}, BasicType::text) == "hello");
    REQUIRE(to_str(ColumnValue{S32(42)}, BasicType::int_) == "42");
    REQUIRE(to_str(ColumnValue{S64(-7LL)}, BasicType::bigint) == "-7");
    REQUIRE(to_str(ColumnValue{S16(0)}, BasicType::smallint) == "0");
    REQUIRE(to_str(ColumnValue{F64(0.0)}, BasicType::double_).length > 0);
}

TEST_CASE("io to_str - uuid format", "[cql.engine.io]") {
    UUID id;
    for (U64 i = 0; i < 16; i++) id.value[i] = 0;
    auto s = to_str(ColumnValue{id}, BasicType::uuid);
    REQUIRE(s.length == 36);
    REQUIRE(s.c_str[8]  == '-');
    REQUIRE(s.c_str[13] == '-');
    REQUIRE(s.c_str[18] == '-');
    REQUIRE(s.c_str[23] == '-');
}

TEST_CASE("io to_str - inet ipv4", "[cql.engine.io]") {
    Inet addr;
    addr.is_v6 = false;
    addr.v4[0] = 127; addr.v4[1] = 0; addr.v4[2] = 0; addr.v4[3] = 1;
    REQUIRE(to_str(ColumnValue{addr}, BasicType::inet) == "127.0.0.1");
}

TEST_CASE("io to_str - duration", "[cql.engine.io]") {
    Duration d{.months = 2, .days = 3, .nanoseconds = 1000000LL};
    REQUIRE(to_str(ColumnValue{d}, BasicType::duration) == "2mo3d1000000ns");
}

TEST_CASE("Type equality", "[cql.types]") {
    REQUIRE(create_basic(BasicType::text) == create_basic(BasicType::text));
    REQUIRE_FALSE(create_basic(BasicType::text) == create_basic(BasicType::int_));
    REQUIRE(create_list(BasicType::int_) == create_list(BasicType::int_));
    REQUIRE_FALSE(create_list(BasicType::int_) == create_list(BasicType::text));
    REQUIRE(create_set(BasicType::uuid) == create_set(BasicType::uuid));
    REQUIRE(create_map(BasicType::text, BasicType::bigint) == create_map(BasicType::text, BasicType::bigint));
    REQUIRE_FALSE(create_map(BasicType::text, BasicType::bigint) == create_map(BasicType::bigint, BasicType::text));
    REQUIRE(create_vector(BasicType::float_, 3) == create_vector(BasicType::float_, 3));
    REQUIRE_FALSE(create_vector(BasicType::float_, 3) == create_vector(BasicType::float_, 4));
    REQUIRE_FALSE(create_list(BasicType::int_) == create_set(BasicType::int_));
}
