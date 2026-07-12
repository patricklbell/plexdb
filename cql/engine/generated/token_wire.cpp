// GENERATED FILE — do not edit by hand.
// Produced by cql/engine/codegen/generate.py from cql/engine/codegen/wire_types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

module;
#include <cstring>

module cql.engine.token.wire;

import cql.engine.io.codec;

import plexdb.base;
import plexdb.os;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

namespace cql::token {
    void write_component(io::Writer w, const ColumnValue& cv, type::Basic dtype, bool is_composite) {
        assert_true(io::can_write_column_value(cv, type::Type{dtype}), "partition key component does not match column type");
        auto emit_prefix = [&](U16 len) {
            if (is_composite) {
                U8 buf[2] = {U8(len >> 8), U8(len & 0xFF)};
                w(buf, 2);
            }
        };
        auto emit_trailer = [&]() {
            if (is_composite) {
                U8 zero = 0;
                w(&zero, 1);
            }
        };

        visit(cv, [&](const auto& v) {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, S16>) {
                emit_prefix(2);
                {
                    U64 bits_ = static_cast<U64>(v);
                    U8  buf[2];
                    buf[0] = U8((bits_ >> 8) & 0xFFu);
                    buf[1] = U8((bits_ >> 0) & 0xFFu);
                    w(buf, 2);
                }

                emit_trailer();

            } else if constexpr (SameAs<T, S32>) {
                emit_prefix(4);
                {
                    U64 bits_ = static_cast<U64>(v);
                    U8  buf[4];
                    buf[0] = U8((bits_ >> 24) & 0xFFu);
                    buf[1] = U8((bits_ >> 16) & 0xFFu);
                    buf[2] = U8((bits_ >> 8) & 0xFFu);
                    buf[3] = U8((bits_ >> 0) & 0xFFu);
                    w(buf, 4);
                }

                emit_trailer();

            } else if constexpr (SameAs<T, S64>) {
                emit_prefix(8);
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

                emit_trailer();

            } else if constexpr (SameAs<T, U8>) {
                emit_prefix(1);
                w(&v, 1);
                emit_trailer();

            } else if constexpr (SameAs<T, F32>) {
                {
                    U32 bits;
                    os::memory_copy(&bits, &v, sizeof(bits));
                    emit_prefix(4);
                    {
                        U64 bits_ = U64(bits);
                        U8  buf[4];
                        buf[0] = U8((bits_ >> 24) & 0xFFu);
                        buf[1] = U8((bits_ >> 16) & 0xFFu);
                        buf[2] = U8((bits_ >> 8) & 0xFFu);
                        buf[3] = U8((bits_ >> 0) & 0xFFu);
                        w(buf, 4);
                    }

                    emit_trailer();
                }

            } else if constexpr (SameAs<T, F64>) {
                {
                    U64 bits;
                    os::memory_copy(&bits, &v, sizeof(bits));
                    emit_prefix(8);
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

                    emit_trailer();
                }

            } else if constexpr (SameAs<T, AutoString8>) {
                emit_prefix(static_cast<U16>(v.length));
                w(reinterpret_cast<const U8*>(v.c_str), v.length);
                emit_trailer();

            } else if constexpr (SameAs<T, Blob>) {
                emit_prefix(static_cast<U16>(v.value.length));
                w(v.value.ptr, v.value.length);
                emit_trailer();

            } else if constexpr (SameAs<T, UUID>) {
                emit_prefix(static_cast<U16>(16));
                w(&v.value[0], 16);
                emit_trailer();

            } else {
                assert_not_implemented("wire encoding for partition key type is not implemented");
            }
        });
    }
}
