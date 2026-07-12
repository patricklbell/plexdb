// GENERATED FILE — do not edit by hand.
// Produced by cql/engine/codegen/generate.py from cql/engine/codegen/wire_types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

module;
#include <coroutine>

module cql.engine.io.codec;
import cql.engine.io.types;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.coroutine;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

namespace cql::io {
    coroutine::Task<ColumnValue> read_column_value(Reader r, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint: {
                S16 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::int_:
            case type::Basic::date: {
                S32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time: {
                S64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::tinyint: {
                U8 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::boolean: {
                U8 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::float_: {
                F32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::double_: {
                F64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));
                co_return {move(value)};

            } break;
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
                AutoString8 value{length};
                co_await r(reinterpret_cast<U8*>(value.c_str), length);
                co_return {move(value)};

            } break;
            case type::Basic::blob:
            case type::Basic::hex: {
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
                Blob value;
                resize(value.value, length);
                co_await r(value.value.ptr, length);
                co_return {move(value)};

            } break;
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                UUID value;
                co_await r(&value.value[0], 16);

                co_return {move(value)};

            } break;
            case type::Basic::inet: {
                Inet value;
                U8   is_v6_raw;
                co_await r(reinterpret_cast<U8*>(&is_v6_raw), sizeof(is_v6_raw));
                value.is_v6 = static_cast<bool>(is_v6_raw);

                if (value.is_v6) {
                    co_await r(&value.v6[0], 16);
                } else {
                    co_await r(&value.v4[0], 4);
                }

                co_return {move(value)};

            } break;
            case type::Basic::varint: {
                VarInt value;
                U8     negative_raw;
                co_await r(reinterpret_cast<U8*>(&negative_raw), sizeof(negative_raw));
                value.negative = static_cast<bool>(negative_raw);

                U64 magnitude_length;
                co_await r(reinterpret_cast<U8*>(&magnitude_length), sizeof(magnitude_length));
                resize(value.magnitude, magnitude_length);
                co_await r(value.magnitude.ptr, magnitude_length);

                co_return {move(value)};

            } break;
            case type::Basic::decimal: {
                Decimal value;
                co_await r(reinterpret_cast<U8*>(&value.scale), sizeof(value.scale));

                U8 unscaled_negative_raw;
                co_await r(reinterpret_cast<U8*>(&unscaled_negative_raw), sizeof(unscaled_negative_raw));
                value.unscaled.negative = static_cast<bool>(unscaled_negative_raw);

                U64 unscaled_magnitude_length;
                co_await r(reinterpret_cast<U8*>(&unscaled_magnitude_length), sizeof(unscaled_magnitude_length));
                resize(value.unscaled.magnitude, unscaled_magnitude_length);
                co_await r(value.unscaled.magnitude.ptr, unscaled_magnitude_length);

                co_return {move(value)};

            } break;
            case type::Basic::duration: {
                Duration value;
                co_await r(reinterpret_cast<U8*>(&value.months), sizeof(value.months));

                co_await r(reinterpret_cast<U8*>(&value.days), sizeof(value.days));

                co_await r(reinterpret_cast<U8*>(&value.nanoseconds), sizeof(value.nanoseconds));

                co_return {move(value)};

            } break;
            case type::Basic::COUNT: {
            } break;
        }
        assert_true(false, "invalid basic type for read");
        co_return {};
    }

    coroutine::Task<void> skip_column_value(Reader r, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint: {
                co_await r(nullptr, sizeof(S16));
                co_return;

            } break;
            case type::Basic::int_:
            case type::Basic::date: {
                co_await r(nullptr, sizeof(S32));
                co_return;

            } break;
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time: {
                co_await r(nullptr, sizeof(S64));
                co_return;

            } break;
            case type::Basic::tinyint: {
                co_await r(nullptr, sizeof(U8));
                co_return;

            } break;
            case type::Basic::boolean: {
                co_await r(nullptr, sizeof(U8));
                co_return;

            } break;
            case type::Basic::float_: {
                co_await r(nullptr, sizeof(F32));
                co_return;

            } break;
            case type::Basic::double_: {
                co_await r(nullptr, sizeof(F64));
                co_return;

            } break;
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
                co_await r(nullptr, length);
                co_return;

            } break;
            case type::Basic::blob:
            case type::Basic::hex: {
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
                co_await r(nullptr, length);
                co_return;

            } break;
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                co_await r(nullptr, 16);

                co_return;

            } break;
            case type::Basic::inet: {
                U8 is_v6_raw;
                co_await r(reinterpret_cast<U8*>(&is_v6_raw), sizeof(is_v6_raw));
                bool is_v6 = static_cast<bool>(is_v6_raw);

                if (is_v6) {
                    co_await r(nullptr, 16);
                } else {
                    co_await r(nullptr, 4);
                }

                co_return;

            } break;
            case type::Basic::varint: {
                co_await r(nullptr, 1);

                U64 magnitude_length;
                co_await r(reinterpret_cast<U8*>(&magnitude_length), sizeof(magnitude_length));
                co_await r(nullptr, magnitude_length);

                co_return;

            } break;
            case type::Basic::decimal: {
                co_await r(nullptr, sizeof(S32));

                co_await r(nullptr, 1);

                U64 unscaled_magnitude_length;
                co_await r(reinterpret_cast<U8*>(&unscaled_magnitude_length), sizeof(unscaled_magnitude_length));
                co_await r(nullptr, unscaled_magnitude_length);

                co_return;

            } break;
            case type::Basic::duration: {
                co_await r(nullptr, sizeof(S32));

                co_await r(nullptr, sizeof(S32));

                co_await r(nullptr, sizeof(S64));

                co_return;

            } break;
            case type::Basic::COUNT:
                break;
        }
        assert_true(false, "invalid basic type in skip_column_value");
    }

    void write_default_column_value(Writer w, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint: {
                S16 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::int_:
            case type::Basic::date: {
                S32 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time: {
                S64 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::tinyint: {
                U8 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::boolean: {
                U8 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::float_: {
                F32 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::double_: {
                F64 value = {};
                w(reinterpret_cast<const U8*>(&value), sizeof(value));

            } break;
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));

            } break;
            case type::Basic::blob:
            case type::Basic::hex: {
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));

            } break;
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                U8 value_zeros[16] = {};
                w(value_zeros, 16);

            } break;
            case type::Basic::inet: {
                U8 is_v6_raw = 0;
                w(reinterpret_cast<const U8*>(&is_v6_raw), sizeof(is_v6_raw));

                U8 v4_zeros[4] = {};
                w(v4_zeros, 4);

            } break;
            case type::Basic::varint: {
                U8 negative_raw = 0;
                w(reinterpret_cast<const U8*>(&negative_raw), sizeof(negative_raw));

                {
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }

            } break;
            case type::Basic::decimal: {
                S32 scale = {};
                w(reinterpret_cast<const U8*>(&scale), sizeof(scale));

                U8 unscaled_negative_raw = 0;
                w(reinterpret_cast<const U8*>(&unscaled_negative_raw), sizeof(unscaled_negative_raw));

                {
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }

            } break;
            case type::Basic::duration: {
                S32 months = {};
                w(reinterpret_cast<const U8*>(&months), sizeof(months));

                S32 days = {};
                w(reinterpret_cast<const U8*>(&days), sizeof(days));

                S64 nanoseconds = {};
                w(reinterpret_cast<const U8*>(&nanoseconds), sizeof(nanoseconds));

            } break;
            case type::Basic::COUNT:
                break;
        }
    }
}
