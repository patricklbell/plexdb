// GENERATED FILE — do not edit by hand.
// Produced by cql/engine/codegen/generate.py from cql/engine/codegen/wire_types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

module cql.engine.io.codec;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

namespace cql::io {
    ColumnValue read_column_value_sync(const U8* src, U16 len, U16* pos, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(S16))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                S16 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::int_:
            case type::Basic::date: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(S32))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                S32 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(S64))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                S64 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::tinyint: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(U8))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                U8 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::boolean: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(U8))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                U8 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::float_: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(F32))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                F32 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::double_: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(F64))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                F64 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(U64))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                U64 length;
                os::memory_copy(&length, src + *pos, sizeof(length));
                *pos += sizeof(length);
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(length)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                AutoString8 value{static_cast<U64>(length)};
                os::memory_copy(value.c_str, src + *pos, length);
                *pos += static_cast<U16>(length);
                return ColumnValue{move(value)};
            }
            case type::Basic::blob:
            case type::Basic::hex: {
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(U64))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                U64 length;
                os::memory_copy(&length, src + *pos, sizeof(length));
                *pos += sizeof(length);
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(length)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                Blob value;
                resize(value.value, length);
                os::memory_copy(value.value.ptr, src + *pos, length);
                *pos += static_cast<U16>(length);
                return ColumnValue{move(value)};
            }
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                UUID value{};
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(16)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                os::memory_copy(&value.value[0], src + *pos, 16);
                *pos += 16;

                return ColumnValue{move(value)};
            }
            case type::Basic::inet: {
                Inet value{};
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(1)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                value.is_v6 = src[*pos] != 0;
                *pos += 1;

                if (value.is_v6) {
                    assert_true(static_cast<U64>(*pos) + (static_cast<U64>(16)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                    os::memory_copy(&value.v6[0], src + *pos, 16);
                    *pos += 16;
                } else {
                    assert_true(static_cast<U64>(*pos) + (static_cast<U64>(4)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                    os::memory_copy(&value.v4[0], src + *pos, 4);
                    *pos += 4;
                }

                return ColumnValue{move(value)};
            }
            case type::Basic::varint: {
                VarInt value{};
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(1)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                value.negative = src[*pos] != 0;
                *pos += 1;

                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(U64))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                U64 magnitude_length;
                os::memory_copy(&magnitude_length, src + *pos, sizeof(magnitude_length));
                *pos += sizeof(magnitude_length);
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(magnitude_length)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                resize(value.magnitude, magnitude_length);
                os::memory_copy(value.magnitude.ptr, src + *pos, magnitude_length);
                *pos += static_cast<U16>(magnitude_length);

                return ColumnValue{move(value)};
            }
            case type::Basic::decimal: {
                Decimal value{};
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(value.scale))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                os::memory_copy(&value.scale, src + *pos, sizeof(value.scale));
                *pos += sizeof(value.scale);

                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(1)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                value.unscaled.negative = src[*pos] != 0;
                *pos += 1;

                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(U64))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                U64 unscaled_magnitude_length;
                os::memory_copy(&unscaled_magnitude_length, src + *pos, sizeof(unscaled_magnitude_length));
                *pos += sizeof(unscaled_magnitude_length);
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(unscaled_magnitude_length)) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                resize(value.unscaled.magnitude, unscaled_magnitude_length);
                os::memory_copy(value.unscaled.magnitude.ptr, src + *pos, unscaled_magnitude_length);
                *pos += static_cast<U16>(unscaled_magnitude_length);

                return ColumnValue{move(value)};
            }
            case type::Basic::duration: {
                Duration value{};
                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(value.months))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                os::memory_copy(&value.months, src + *pos, sizeof(value.months));
                *pos += sizeof(value.months);

                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(value.days))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                os::memory_copy(&value.days, src + *pos, sizeof(value.days));
                *pos += sizeof(value.days);

                assert_true(static_cast<U64>(*pos) + (static_cast<U64>(sizeof(value.nanoseconds))) <= static_cast<U64>(len), "read_column_value_sync: field exceeds buffer bounds");

                os::memory_copy(&value.nanoseconds, src + *pos, sizeof(value.nanoseconds));
                *pos += sizeof(value.nanoseconds);

                return ColumnValue{move(value)};
            }
            case type::Basic::COUNT:
                break;
        }
        assert_true(false, "invalid basic type in read_column_value_sync");
        return ColumnValue{Null{}};
    }
}
