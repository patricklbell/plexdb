#include <catch2/catch_test_macros.hpp>

#include <plexdb/test_macros/test_macros.h>

import plexdb.base;
import plexdb.coroutine;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
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
        return [this](U8* dst, U64 size) -> coroutine::Task<void> {
            if (dst) for (U64 i = 0; i < size; i++) dst[i] = data[cursor + i];
            cursor += size;
            co_return;
        };
    }
};

IO_TEST_CASE("io roundtrip - scalar types", "[cql.engine.io]") {
    SECTION("text") {
        Buffer buf;
        ColumnValue in{AutoString8("hello world")};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::text));
        auto out = co_await read_column_value(buf.reader(), type::Basic::text);
        REQUIRE(type_matches_tag<AutoString8>(out));
        REQUIRE(get<AutoString8>(out) == "hello world");
    }

    SECTION("empty text") {
        Buffer buf;
        ColumnValue in{AutoString8("")};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::text));
        auto out = co_await read_column_value(buf.reader(), type::Basic::text);
        REQUIRE(type_matches_tag<AutoString8>(out));
        REQUIRE(get<AutoString8>(out).length == 0);
    }

    SECTION("int") {
        Buffer buf;
        ColumnValue in{S32(-42)};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::int_));
        auto out = co_await read_column_value(buf.reader(), type::Basic::int_);
        REQUIRE(type_matches_tag<S32>(out));
        REQUIRE(get<S32>(out) == -42);
    }

    SECTION("bigint") {
        Buffer buf;
        ColumnValue in{S64(9999999999LL)};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::bigint));
        auto out = co_await read_column_value(buf.reader(), type::Basic::bigint);
        REQUIRE(type_matches_tag<S64>(out));
        REQUIRE(get<S64>(out) == 9999999999LL);
    }

    SECTION("smallint") {
        Buffer buf;
        ColumnValue in{S16(-32000)};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::smallint));
        auto out = co_await read_column_value(buf.reader(), type::Basic::smallint);
        REQUIRE(type_matches_tag<S16>(out));
        REQUIRE(get<S16>(out) == -32000);
    }

    SECTION("boolean") {
        Buffer buf;
        ColumnValue in_t{U8(1)};
        write_column_value(buf.writer(), in_t, type::create_basic(type::Basic::boolean));
        auto out_t = co_await read_column_value(buf.reader(), type::Basic::boolean);
        REQUIRE(type_matches_tag<U8>(out_t));
        REQUIRE(get<U8>(out_t) == 1);

        Buffer buf2;
        ColumnValue in_f{U8(0)};
        write_column_value(buf2.writer(), in_f, type::create_basic(type::Basic::boolean));
        auto out_f = co_await read_column_value(buf2.reader(), type::Basic::boolean);
        REQUIRE(get<U8>(out_f) == 0);
    }

    SECTION("float") {
        Buffer buf;
        ColumnValue in{F32(3.14f)};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::float_));
        auto out = co_await read_column_value(buf.reader(), type::Basic::float_);
        REQUIRE(type_matches_tag<F32>(out));
        REQUIRE(get<F32>(out) == 3.14f);
    }

    SECTION("double") {
        Buffer buf;
        ColumnValue in{F64(-2.71828)};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::double_));
        auto out = co_await read_column_value(buf.reader(), type::Basic::double_);
        REQUIRE(type_matches_tag<F64>(out));
        REQUIRE(get<F64>(out) == -2.71828);
    }

    SECTION("uuid") {
        Buffer buf;
        UUID id;
        for (U64 i = 0; i < 16; i++) id.value[i] = static_cast<U8>(i + 1);
        ColumnValue in{id};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::uuid));
        auto out = co_await read_column_value(buf.reader(), type::Basic::uuid);
        REQUIRE(type_matches_tag<UUID>(out));
        REQUIRE(get<UUID>(out) == id);
    }
}

IO_TEST_CASE("io roundtrip - inet", "[cql.engine.io]") {
    SECTION("ipv4") {
        Buffer buf;
        Inet addr;
        addr.is_v6 = false;
        addr.v4[0] = 192; addr.v4[1] = 168; addr.v4[2] = 1; addr.v4[3] = 42;
        ColumnValue in{addr};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::inet));
        auto out = co_await read_column_value(buf.reader(), type::Basic::inet);
        REQUIRE(type_matches_tag<Inet>(out));
        REQUIRE(get<Inet>(out) == addr);
    }

    SECTION("ipv6") {
        Buffer buf;
        Inet addr;
        addr.is_v6 = true;
        for (int i = 0; i < 16; i++) addr.v6[i] = static_cast<U8>(i * 0x11);
        ColumnValue in{addr};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::inet));
        auto out = co_await read_column_value(buf.reader(), type::Basic::inet);
        REQUIRE(type_matches_tag<Inet>(out));
        REQUIRE(get<Inet>(out) == addr);
    }
}

IO_TEST_CASE("io roundtrip - varint", "[cql.engine.io]") {
    SECTION("positive") {
        Buffer buf;
        VarInt val;
        val.negative = false;
        push_back(val.magnitude, U8(0x01));
        push_back(val.magnitude, U8(0x00));
        ColumnValue in{val};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::varint));
        auto out = co_await read_column_value(buf.reader(), type::Basic::varint);
        REQUIRE(type_matches_tag<VarInt>(out));
        REQUIRE(get<VarInt>(out) == val);
    }

    SECTION("negative") {
        Buffer buf;
        VarInt val;
        val.negative = true;
        push_back(val.magnitude, U8(0xFF));
        ColumnValue in{val};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::varint));
        auto out = co_await read_column_value(buf.reader(), type::Basic::varint);
        REQUIRE(type_matches_tag<VarInt>(out));
        REQUIRE(get<VarInt>(out) == val);
    }

    SECTION("zero") {
        Buffer buf;
        VarInt val;
        val.negative = false;
        ColumnValue in{val};
        write_column_value(buf.writer(), in, type::create_basic(type::Basic::varint));
        auto out = co_await read_column_value(buf.reader(), type::Basic::varint);
        REQUIRE(type_matches_tag<VarInt>(out));
        REQUIRE(get<VarInt>(out).magnitude.length == 0);
    }
}

IO_TEST_CASE("io roundtrip - decimal", "[cql.engine.io]") {
    Buffer buf;
    Decimal val;
    val.scale = 3;
    val.unscaled.negative = false;
    push_back(val.unscaled.magnitude, U8(0x27));
    push_back(val.unscaled.magnitude, U8(0x0F));
    ColumnValue in{val};
    write_column_value(buf.writer(), in, type::create_basic(type::Basic::decimal));
    auto out = co_await read_column_value(buf.reader(), type::Basic::decimal);
    REQUIRE(type_matches_tag<Decimal>(out));
    REQUIRE(get<Decimal>(out) == val);
}

IO_TEST_CASE("io roundtrip - duration", "[cql.engine.io]") {
    Buffer buf;
    Duration val{.months = 1, .days = 15, .nanoseconds = 3600000000000LL};
    ColumnValue in{val};
    write_column_value(buf.writer(), in, type::create_basic(type::Basic::duration));
    auto out = co_await read_column_value(buf.reader(), type::Basic::duration);
    REQUIRE(type_matches_tag<Duration>(out));
    REQUIRE(get<Duration>(out) == val);
}

IO_TEST_CASE("io roundtrip - collections", "[cql.engine.io]") {
    SECTION("list<text>") {
        Buffer buf;
        type::Type t = type::create_list(type::Basic::text);
        DynamicArray<AutoString8> arr{};
        push_back(arr, AutoString8("alpha"));
        push_back(arr, AutoString8("beta"));
        push_back(arr, AutoString8("gamma"));
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = co_await read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<AutoString8>>(out));
        auto& got = get<DynamicArray<AutoString8>>(out);
        REQUIRE(got.length == 3);
        REQUIRE(got[0] == "alpha");
        REQUIRE(got[1] == "beta");
        REQUIRE(got[2] == "gamma");
    }

    SECTION("list<int>") {
        Buffer buf;
        type::Type t = type::create_list(type::Basic::int_);
        DynamicArray<S32> arr{};
        push_back(arr, S32(1));
        push_back(arr, S32(-99));
        push_back(arr, S32(0));
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = co_await read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<S32>>(out));
        auto& got = get<DynamicArray<S32>>(out);
        REQUIRE(got.length == 3);
        REQUIRE(got[0] == 1);
        REQUIRE(got[1] == -99);
        REQUIRE(got[2] == 0);
    }

    SECTION("empty list") {
        Buffer buf;
        type::Type t = type::create_list(type::Basic::bigint);
        DynamicArray<S64> arr{};
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = co_await read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<S64>>(out));
        REQUIRE(get<DynamicArray<S64>>(out).length == 0);
    }

    SECTION("vector<float>") {
        Buffer buf;
        type::Type t = type::create_vector(type::Basic::float_, 3);
        DynamicArray<F32> arr{};
        push_back(arr, F32(1.0f));
        push_back(arr, F32(2.5f));
        push_back(arr, F32(-0.5f));
        ColumnValue in{move(arr)};
        write_column_value(buf.writer(), in, t);
        auto out = co_await read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicArray<F32>>(out));
        auto& got = get<DynamicArray<F32>>(out);
        REQUIRE(got.length == 3);
        REQUIRE(got[0] == 1.0f);
        REQUIRE(got[1] == 2.5f);
        REQUIRE(got[2] == -0.5f);
    }

    SECTION("set<bigint>") {
        Buffer buf;
        type::Type t = type::create_set(type::Basic::bigint);
        DynamicSet<S64> s{};
        insert(s, S64(100LL));
        insert(s, S64(200LL));
        insert(s, S64(300LL));
        ColumnValue in{move(s)};
        write_column_value(buf.writer(), in, t);
        auto out = co_await read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicSet<S64>>(out));
        REQUIRE(length(get<DynamicSet<S64>>(out)) == 3);
    }

    SECTION("map<text,bigint>") {
        Buffer buf;
        type::Type t = type::create_map(type::Basic::text, type::Basic::bigint);
        DynamicMap<AutoString8, S64> m{};
        insert(m, AutoString8("key1"), S64(42LL));
        insert(m, AutoString8("key2"), S64(-1LL));
        ColumnValue in{move(m)};
        write_column_value(buf.writer(), in, t);
        auto out = co_await read_column_value(buf.reader(), t);
        REQUIRE(type_matches_tag<DynamicMap<AutoString8, S64>>(out));
        REQUIRE(length(get<DynamicMap<AutoString8, S64>>(out)) == 2);
    }
}

IO_TEST_CASE("io write_default_column_value", "[cql.engine.io]") {
    auto check_default = [](type::Basic dtype) -> coroutine::Task<void> {
        Buffer buf;
        write_default_column_value(buf.writer(), dtype);
        co_await read_column_value(buf.reader(), dtype);
        REQUIRE(buf.cursor == buf.data.length);
    };

    co_await check_default(type::Basic::text);
    co_await check_default(type::Basic::int_);
    co_await check_default(type::Basic::bigint);
    co_await check_default(type::Basic::smallint);
    co_await check_default(type::Basic::boolean);
    co_await check_default(type::Basic::float_);
    co_await check_default(type::Basic::double_);
    co_await check_default(type::Basic::uuid);
    co_await check_default(type::Basic::blob);
    co_await check_default(type::Basic::inet);
    co_await check_default(type::Basic::varint);
    co_await check_default(type::Basic::decimal);
    co_await check_default(type::Basic::duration);

    SECTION("default int is zero") {
        Buffer buf;
        write_default_column_value(buf.writer(), type::Basic::int_);
        auto out = co_await read_column_value(buf.reader(), type::Basic::int_);
        REQUIRE(get<S32>(out) == 0);
    }

    SECTION("default inet is ipv4 zero") {
        Buffer buf;
        write_default_column_value(buf.writer(), type::Basic::inet);
        auto out = co_await read_column_value(buf.reader(), type::Basic::inet);
        REQUIRE(type_matches_tag<Inet>(out));
        REQUIRE(get<Inet>(out).is_v6 == false);
    }
}

TEST_CASE("io to_str - scalars", "[cql.engine.io]") {
    REQUIRE(to_str(ColumnValue{AutoString8("hello")}, type::Basic::text) == "hello");
    REQUIRE(to_str(ColumnValue{S32(42)}, type::Basic::int_) == "42");
    REQUIRE(to_str(ColumnValue{S64(-7LL)}, type::Basic::bigint) == "-7");
    REQUIRE(to_str(ColumnValue{S16(0)}, type::Basic::smallint) == "0");
    REQUIRE(to_str(ColumnValue{F64(0.0)}, type::Basic::double_).length > 0);
}

TEST_CASE("io to_str - uuid format", "[cql.engine.io]") {
    UUID id;
    for (U64 i = 0; i < 16; i++) id.value[i] = 0;
    auto s = to_str(ColumnValue{id}, type::Basic::uuid);
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
    REQUIRE(to_str(ColumnValue{addr}, type::Basic::inet) == "127.0.0.1");
}

TEST_CASE("io to_str - duration", "[cql.engine.io]") {
    Duration d{.months = 2, .days = 3, .nanoseconds = 1000000LL};
    REQUIRE(to_str(ColumnValue{d}, type::Basic::duration) == "2mo3d1000000ns");
}

TEST_CASE("Type equality", "[cql.types]") {
    REQUIRE(type::create_basic(type::Basic::text) == type::create_basic(type::Basic::text));
    REQUIRE_FALSE(type::create_basic(type::Basic::text) == type::create_basic(type::Basic::int_));
    REQUIRE(type::create_list(type::Basic::int_) == type::create_list(type::Basic::int_));
    REQUIRE_FALSE(type::create_list(type::Basic::int_) == type::create_list(type::Basic::text));
    REQUIRE(type::create_set(type::Basic::uuid) == type::create_set(type::Basic::uuid));
    REQUIRE(type::create_map(type::Basic::text, type::Basic::bigint) == type::create_map(type::Basic::text, type::Basic::bigint));
    REQUIRE_FALSE(type::create_map(type::Basic::text, type::Basic::bigint) == type::create_map(type::Basic::bigint, type::Basic::text));
    REQUIRE(type::create_vector(type::Basic::float_, 3) == type::create_vector(type::Basic::float_, 3));
    REQUIRE_FALSE(type::create_vector(type::Basic::float_, 3) == type::create_vector(type::Basic::float_, 4));
    REQUIRE_FALSE(type::create_list(type::Basic::int_) == type::create_set(type::Basic::int_));
}
