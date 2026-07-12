// GENERATED FILE — do not edit by hand.
// Produced by cql/engine/codegen/generate.py from cql/engine/codegen/wire_types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

export module cql.engine.io.codec.wire;

import cql.engine.io.types;

import plexdb.base;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

export namespace cql::io {
    template<typename T>
    void write_typed_basic(Writer w, const T& src, [[maybe_unused]] type::Basic dtype) {
        if constexpr (SameAs<T, S16>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));

        } else if constexpr (SameAs<T, S32>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));

        } else if constexpr (SameAs<T, S64>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));

        } else if constexpr (SameAs<T, U8>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));

        } else if constexpr (SameAs<T, F32>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));

        } else if constexpr (SameAs<T, F64>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));

        } else if constexpr (SameAs<T, AutoString8>) {
            String8 s{src};
            w(reinterpret_cast<const U8*>(&s.length), sizeof(s.length));
            w(reinterpret_cast<const U8*>(s.data), s.length);

        } else if constexpr (SameAs<T, Blob>) {
            w(reinterpret_cast<const U8*>(&src.value.length), sizeof(src.value.length));
            w(src.value.ptr, src.value.length);

        } else if constexpr (SameAs<T, UUID>) {
            w(&src.value[0], 16);

        } else if constexpr (SameAs<T, Inet>) {
            U8 is_v6_raw = src.is_v6 ? 1 : 0;
            w(reinterpret_cast<const U8*>(&is_v6_raw), sizeof(is_v6_raw));

            if (src.is_v6) {
                w(&src.v6[0], 16);
            } else {
                w(&src.v4[0], 4);
            }

        } else if constexpr (SameAs<T, VarInt>) {
            U8 negative_raw = src.negative ? 1 : 0;
            w(reinterpret_cast<const U8*>(&negative_raw), sizeof(negative_raw));

            w(reinterpret_cast<const U8*>(&src.magnitude.length), sizeof(src.magnitude.length));
            w(src.magnitude.ptr, src.magnitude.length);

        } else if constexpr (SameAs<T, Decimal>) {
            w(reinterpret_cast<const U8*>(&src.scale), sizeof(src.scale));

            U8 unscaled_negative_raw = src.unscaled.negative ? 1 : 0;
            w(reinterpret_cast<const U8*>(&unscaled_negative_raw), sizeof(unscaled_negative_raw));

            w(reinterpret_cast<const U8*>(&src.unscaled.magnitude.length), sizeof(src.unscaled.magnitude.length));
            w(src.unscaled.magnitude.ptr, src.unscaled.magnitude.length);

        } else if constexpr (SameAs<T, Duration>) {
            w(reinterpret_cast<const U8*>(&src.months), sizeof(src.months));

            w(reinterpret_cast<const U8*>(&src.days), sizeof(src.days));

            w(reinterpret_cast<const U8*>(&src.nanoseconds), sizeof(src.nanoseconds));

        } else {
            static_assert(!SameAs<T, T>, "missing type case");
        }
    }

    bool is_literal_integer_family(type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint:
            case type::Basic::int_:
            case type::Basic::date:
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time:
            case type::Basic::tinyint:
                return true;
            default:
                return false;
        }
    }
    bool is_literal_float_family(type::Basic dtype) {
        switch (dtype) {
            case type::Basic::float_:
            case type::Basic::double_:
                return true;
            default:
                return false;
        }
    }

    template<typename T>
    bool can_write_typed_basic_as_column_value(type::Basic dtype) {
        if constexpr (SameAs<RemoveCV<T>, S64>) {
            return is_literal_integer_family(dtype);
        } else if constexpr (SameAs<RemoveCV<T>, F64>) {
            return is_literal_float_family(dtype);
        } else if constexpr (SameAs<RemoveCV<T>, bool>) {
            return dtype == type::Basic::boolean;
        } else if constexpr (SameAs<RemoveCV<T>, Null>) {
            return true;
        } else if constexpr (SameAs<RemoveCV<T>, Unset>) {
            return false;
        } else if constexpr (SameAs<RemoveCV<T>, Hex>) {
            return dtype == type::Basic::hex;
        } else if constexpr (SameAs<RemoveCV<T>, S16>) {
            return dtype == type::Basic::smallint;
        } else if constexpr (SameAs<RemoveCV<T>, S32>) {
            return dtype == type::Basic::int_ || dtype == type::Basic::date;
        } else if constexpr (SameAs<RemoveCV<T>, U8>) {
            return dtype == type::Basic::tinyint || dtype == type::Basic::boolean;
        } else if constexpr (SameAs<RemoveCV<T>, F32>) {
            return dtype == type::Basic::float_;
        } else if constexpr (SameAs<RemoveCV<T>, AutoString8>) {
            return dtype == type::Basic::text || dtype == type::Basic::varchar || dtype == type::Basic::ascii;
        } else if constexpr (SameAs<RemoveCV<T>, Blob>) {
            return dtype == type::Basic::blob || dtype == type::Basic::hex;
        } else if constexpr (SameAs<RemoveCV<T>, UUID>) {
            return dtype == type::Basic::uuid || dtype == type::Basic::timeuuid;
        } else if constexpr (SameAs<RemoveCV<T>, Inet>) {
            return dtype == type::Basic::inet;
        } else if constexpr (SameAs<RemoveCV<T>, VarInt>) {
            return dtype == type::Basic::varint;
        } else if constexpr (SameAs<RemoveCV<T>, Decimal>) {
            return dtype == type::Basic::decimal;
        } else if constexpr (SameAs<RemoveCV<T>, Duration>) {
            return dtype == type::Basic::duration;
        } else {
            static_assert(!SameAs<T, T>, "missing type case");
            return false;
        }
    }

    ColumnValue narrow_s64_literal(S64 v, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::smallint:
                return ColumnValue{static_cast<S16>(v)};
            case type::Basic::int_:
            case type::Basic::date:
                return ColumnValue{static_cast<S32>(v)};
            case type::Basic::tinyint:
                return ColumnValue{static_cast<U8>(static_cast<S8>(v))};
            default:
                return ColumnValue{static_cast<S64>(v)};
        }
    }
    ColumnValue narrow_f64_literal(F64 v, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::float_:
                return ColumnValue{static_cast<F32>(v)};
            default:
                return ColumnValue{static_cast<F64>(v)};
        }
    }
}
