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
    ColumnValue read_column_value_sync(const U8* src, U16, U16* pos, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint: {
                S16 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::int_:
            case type::Basic::date: {
                S32 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time: {
                S64 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::tinyint: {
                U8 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::boolean: {
                U8 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::float_: {
                F32 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::double_: {
                F64 value;
                os::memory_copy(&value, src + *pos, sizeof(value));
                *pos += sizeof(value);
                return ColumnValue{value};
            }
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                U64 length;
                os::memory_copy(&length, src + *pos, sizeof(length));
                *pos += sizeof(length);
                AutoString8 value{static_cast<U64>(length)};
                os::memory_copy(value.c_str, src + *pos, length);
                *pos += static_cast<U16>(length);
                return ColumnValue{move(value)};
            }
            case type::Basic::blob:
            case type::Basic::hex: {
                U64 length;
                os::memory_copy(&length, src + *pos, sizeof(length));
                *pos += sizeof(length);
                Blob value;
                resize(value.value, length);
                os::memory_copy(value.value.ptr, src + *pos, length);
                *pos += static_cast<U16>(length);
                return ColumnValue{move(value)};
            }
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                UUID value{};
                os::memory_copy(&value.value[0], src + *pos, 16);
                *pos += 16;

                return ColumnValue{move(value)};
            }
            case type::Basic::inet: {
                Inet value{};
                value.is_v6 = src[*pos] != 0;
                *pos += 1;

                if (value.is_v6) {
                    os::memory_copy(&value.v6[0], src + *pos, 16);
                    *pos += 16;
                } else {
                    os::memory_copy(&value.v4[0], src + *pos, 4);
                    *pos += 4;
                }

                return ColumnValue{move(value)};
            }
            case type::Basic::varint: {
                VarInt value{};
                value.negative = src[*pos] != 0;
                *pos += 1;

                U64 magnitude_length;
                os::memory_copy(&magnitude_length, src + *pos, sizeof(magnitude_length));
                *pos += sizeof(magnitude_length);
                resize(value.magnitude, magnitude_length);
                os::memory_copy(value.magnitude.ptr, src + *pos, magnitude_length);
                *pos += static_cast<U16>(magnitude_length);

                return ColumnValue{move(value)};
            }
            case type::Basic::decimal: {
                Decimal value{};
                os::memory_copy(&value.scale, src + *pos, sizeof(value.scale));
                *pos += sizeof(value.scale);

                value.unscaled.negative = src[*pos] != 0;
                *pos += 1;

                U64 unscaled_magnitude_length;
                os::memory_copy(&unscaled_magnitude_length, src + *pos, sizeof(unscaled_magnitude_length));
                *pos += sizeof(unscaled_magnitude_length);
                resize(value.unscaled.magnitude, unscaled_magnitude_length);
                os::memory_copy(value.unscaled.magnitude.ptr, src + *pos, unscaled_magnitude_length);
                *pos += static_cast<U16>(unscaled_magnitude_length);

                return ColumnValue{move(value)};
            }
            case type::Basic::duration: {
                Duration value{};
                os::memory_copy(&value.months, src + *pos, sizeof(value.months));
                *pos += sizeof(value.months);

                os::memory_copy(&value.days, src + *pos, sizeof(value.days));
                *pos += sizeof(value.days);

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
