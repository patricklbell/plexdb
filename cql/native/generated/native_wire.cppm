// GENERATED FILE — do not edit by hand.
// Produced by cql/codegen/generate.py from cql/codegen/types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

export module cql.native.wire;

import cql.engine.io.types;
import cql.column_types;
import cql.column_types.twos_complement;
import cql.column_types.vint;

import plexdb.base;
import plexdb.os;

import cql.engine.statements;

using namespace plexdb;
using namespace cql::column_types;

export namespace cql::native::wire {
    // CQL native protocol (client-facing) wire shape for one value's body — big-endian,
    // no length prefix (the caller writes the [int] length itself, from
    // native_basic_body_length, framing this). Distinct from cql::io::write_typed_basic
    // (host-native storage layout) and cql::token::write_component (partition-key hashing
    // input) — all three are generated from the same types.json but differ in byte
    // order/shape where Cassandra's own formats differ. Depends only on cql.column_types (the
    // value shapes) and cql.engine.statements (Term/Literal) — not on cql.engine as a whole.
    template<typename T>
    U64 native_basic_body_length([[maybe_unused]] Basic dtype, const T& v) {
        if constexpr (SameAs<T, S16>) {
            return 2;
        } else if constexpr (SameAs<T, S32>) {
            return 4;
        } else if constexpr (SameAs<T, S64>) {
            return 8;
        } else if constexpr (SameAs<T, U8>) {
            return 1;
        } else if constexpr (SameAs<T, F32>) {
            return 4;
        } else if constexpr (SameAs<T, F64>) {
            return 8;
        } else if constexpr (SameAs<T, AutoString8>) {
            return v.length;
        } else if constexpr (SameAs<T, Blob>) {
            return v.value.length;
        } else if constexpr (SameAs<T, UUID>) {
            return 16;
        } else if constexpr (SameAs<T, Inet>) {
            return (v.is_v6 ? 16 : 4);
        } else if constexpr (SameAs<T, VarInt>) {
            return twos_complement::length(v.negative, v.magnitude.ptr, v.magnitude.length);
        } else if constexpr (SameAs<T, Decimal>) {
            return (4 + twos_complement::length(v.unscaled.negative, v.unscaled.magnitude.ptr, v.unscaled.magnitude.length));
        } else if constexpr (SameAs<T, Duration>) {
            return (vint::encoded_length(static_cast<S64>(v.months)) + vint::encoded_length(static_cast<S64>(v.days)) + vint::encoded_length(v.nanoseconds));
        } else if constexpr (SameAs<T, Hex>) {
            return v.value.length;
        } else {
            static_assert(!SameAs<T, T>, "missing native wire length case");
        }
    }

    template<typename T>
    void write_native_basic_body(io::Writer w, [[maybe_unused]] Basic dtype, const T& v) {
        if constexpr (SameAs<T, S16>) {
            {
                U64 bits_ = static_cast<U64>(v);
                U8  buf[2];
                buf[0] = U8((bits_ >> 8) & 0xFFu);
                buf[1] = U8((bits_ >> 0) & 0xFFu);
                w(buf, 2);
            }

        } else if constexpr (SameAs<T, S32>) {
            {
                U64 bits_ = static_cast<U64>(v);
                U8  buf[4];
                buf[0] = U8((bits_ >> 24) & 0xFFu);
                buf[1] = U8((bits_ >> 16) & 0xFFu);
                buf[2] = U8((bits_ >> 8) & 0xFFu);
                buf[3] = U8((bits_ >> 0) & 0xFFu);
                w(buf, 4);
            }

        } else if constexpr (SameAs<T, S64>) {
            {
                U64 bits_ = static_cast<U64>(v);
                U8  buf[8];
                buf[0] = U8((bits_ >> 56) & 0xFFu);
                buf[1] = U8((bits_ >> 48) & 0xFFu);
                buf[2] = U8((bits_ >> 40) & 0xFFu);
                buf[3] = U8((bits_ >> 32) & 0xFFu);
                buf[4] = U8((bits_ >> 24) & 0xFFu);
                buf[5] = U8((bits_ >> 16) & 0xFFu);
                buf[6] = U8((bits_ >> 8) & 0xFFu);
                buf[7] = U8((bits_ >> 0) & 0xFFu);
                w(buf, 8);
            }

        } else if constexpr (SameAs<T, U8>) {
            w(&v, 1);

        } else if constexpr (SameAs<T, F32>) {
            {
                U32 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                {
                    U64 bits_ = U64(bits);
                    U8  buf[4];
                    buf[0] = U8((bits_ >> 24) & 0xFFu);
                    buf[1] = U8((bits_ >> 16) & 0xFFu);
                    buf[2] = U8((bits_ >> 8) & 0xFFu);
                    buf[3] = U8((bits_ >> 0) & 0xFFu);
                    w(buf, 4);
                }
            }

        } else if constexpr (SameAs<T, F64>) {
            {
                U64 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                {
                    U64 bits_ = U64(bits);
                    U8  buf[8];
                    buf[0] = U8((bits_ >> 56) & 0xFFu);
                    buf[1] = U8((bits_ >> 48) & 0xFFu);
                    buf[2] = U8((bits_ >> 40) & 0xFFu);
                    buf[3] = U8((bits_ >> 32) & 0xFFu);
                    buf[4] = U8((bits_ >> 24) & 0xFFu);
                    buf[5] = U8((bits_ >> 16) & 0xFFu);
                    buf[6] = U8((bits_ >> 8) & 0xFFu);
                    buf[7] = U8((bits_ >> 0) & 0xFFu);
                    w(buf, 8);
                }
            }

        } else if constexpr (SameAs<T, AutoString8>) {
            w(reinterpret_cast<const U8*>(v.c_str), v.length);

        } else if constexpr (SameAs<T, Blob>) {
            w(v.value.ptr, v.value.length);

        } else if constexpr (SameAs<T, UUID>) {
            w(&v.value[0], 16);

        } else if constexpr (SameAs<T, Inet>) {
            if (v.is_v6) {
                w(&v.v6[0], 16);
            } else {
                w(&v.v4[0], 4);
            }

        } else if constexpr (SameAs<T, VarInt>) {
            twos_complement::write(w, v.negative, v.magnitude.ptr, v.magnitude.length);

        } else if constexpr (SameAs<T, Decimal>) {
            {
                {
                    U64 bits_ = static_cast<U64>(static_cast<U32>(v.scale));
                    U8  buf[4];
                    buf[0] = U8((bits_ >> 24) & 0xFFu);
                    buf[1] = U8((bits_ >> 16) & 0xFFu);
                    buf[2] = U8((bits_ >> 8) & 0xFFu);
                    buf[3] = U8((bits_ >> 0) & 0xFFu);
                    w(buf, 4);
                }

                twos_complement::write(w, v.unscaled.negative, v.unscaled.magnitude.ptr, v.unscaled.magnitude.length);
            }

        } else if constexpr (SameAs<T, Duration>) {
            {
                vint::write(w, static_cast<S64>(v.months));
                vint::write(w, static_cast<S64>(v.days));
                vint::write(w, v.nanoseconds);
            }

        } else if constexpr (SameAs<T, Hex>) {
            w(v.value.ptr, v.value.length);
        } else {
            static_assert(!SameAs<T, T>, "missing native wire write case");
        }
    }

    // Decodes one [value] (the [int] length prefix through the value body) into the most
    // specific Term available: Literal for genuine CQL literal kinds, ColumnValue directly
    // for Inet/Decimal (no literal syntax exists for either — see resolve_literal_scalar).
    Term read_native_basic_value_as_term(const U8*& p, const U8* end, Basic dtype);
}
