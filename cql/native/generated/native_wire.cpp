// GENERATED FILE — do not edit by hand.
// Produced by cql/codegen/generate.py from cql/codegen/types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

module;
#include <cstring>

module cql.native.wire;

import cql.column_types;
import cql.column_types.twos_complement;
import cql.column_types.vint;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.engine.statements;

using namespace plexdb;
using namespace cql::column_types;

namespace cql::native::wire {
    Term read_native_basic_value_as_term(const U8*& p, const U8* end, Basic dtype) {
        assert_true(p + 4 <= end, "truncated value length");
        S32 len = S32((U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | U32(p[3]));
        p += 4;
        if (len == -2) {
            return Term{.value = Literal{.value = Unset{}}};
        }
        if (len < 0) {
            return Term{.value = Literal{.value = Null{}}};
        }
        assert_true(p + len <= end, "value body truncated");
        const U8* val = p;
        p += len;

        switch (dtype) {
            case Basic::smallint: {
                return Term{.value = Literal{.value = S64(S16((U16(val[0]) << 8) | U16(val[1])))}};

            } break;
            case Basic::int_:
            case Basic::date: {
                return Term{.value = Literal{.value = S64(S32((U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3])))}};

            } break;
            case Basic::bigint:
            case Basic::timestamp:
            case Basic::counter:
            case Basic::time: {
                return Term{.value = Literal{.value = S64((U64(val[0]) << 56) | (U64(val[1]) << 48) | (U64(val[2]) << 40) | (U64(val[3]) << 32) | (U64(val[4]) << 24) | (U64(val[5]) << 16) | (U64(val[6]) << 8) | U64(val[7]))}};

            } break;
            case Basic::tinyint: {
                return Term{.value = Literal{.value = S64(S8(val[0]))}};

            } break;
            case Basic::boolean: {
                return Term{.value = Literal{.value = (len > 0 && val[0] != 0)}};

            } break;
            case Basic::float_: {
                {
                    U32 bits = (U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3]);
                    F32 fv;
                    os::memory_copy(&fv, &bits, sizeof(fv));
                    return Term{.value = Literal{.value = F64(fv)}};
                }

            } break;
            case Basic::double_: {
                {
                    U64 bits = (U64(val[0]) << 56) | (U64(val[1]) << 48) | (U64(val[2]) << 40) | (U64(val[3]) << 32) | (U64(val[4]) << 24) | (U64(val[5]) << 16) | (U64(val[6]) << 8) | U64(val[7]);
                    F64 dv;
                    os::memory_copy(&dv, &bits, sizeof(dv));
                    return Term{.value = Literal{.value = dv}};
                }

            } break;
            case Basic::text:
            case Basic::varchar:
            case Basic::ascii: {
                return Term{.value = Literal{.value = AutoString8(val, U64(len))}};

            } break;
            case Basic::uuid:
            case Basic::timeuuid: {
                {
                    UUID uuid{};
                    os::memory_copy(&uuid.value[0], val, 16);
                    return Term{.value = Literal{.value = uuid}};
                }

            } break;
            case Basic::inet: {
                {
                    Inet inet{};
                    inet.is_v6 = (len == 16);
                    if (inet.is_v6) {
                        os::memory_copy(&inet.v6[0], val, 16);
                    } else {
                        os::memory_copy(&inet.v4[0], val, 4);
                    }
                    return Term{.value = ColumnValue{inet}};
                }

            } break;
            case Basic::varint: {
                {
                    twos_complement::Decoded d_ = twos_complement::read(val, U64(len));
                    VarInt                   vi_{};
                    vi_.negative  = d_.negative;
                    vi_.magnitude = move(d_.magnitude);
                    return Term{.value = Literal{.value = move(vi_)}};
                }

            } break;
            case Basic::decimal: {
                {
                    assert_true(len >= 4, "truncated decimal scale");
                    S32                      scale_ = S32((U32(val[0]) << 24) | (U32(val[1]) << 16) | (U32(val[2]) << 8) | U32(val[3]));
                    twos_complement::Decoded d_     = twos_complement::read(val + 4, U64(len) - 4);
                    VarInt                   un_{};
                    un_.negative  = d_.negative;
                    un_.magnitude = move(d_.magnitude);
                    return Term{.value = ColumnValue{Decimal{scale_, move(un_)}}};
                }

            } break;
            case Basic::duration: {
                {
                    const U8* dp   = val;
                    const U8* dend = val + len;
                    Duration  d{};
                    d.months      = S32(vint::read(dp, dend));
                    d.days        = S32(vint::read(dp, dend));
                    d.nanoseconds = vint::read(dp, dend);
                    return Term{.value = Literal{.value = d}};
                }

            } break;
            case Basic::blob: {
                Blob b{};
                resize(b.value, U64(len));
                os::memory_copy(b.value.ptr, val, U64(len));
                return Term{.value = Literal{.value = move(b)}};
            } break;
            case Basic::hex: {
                Hex h{};
                resize(h.value, U64(len));
                os::memory_copy(h.value.ptr, val, U64(len));
                return Term{.value = Literal{.value = move(h)}};
            } break;
            case Basic::COUNT:
                break;
        }
        assert_true(false, "invalid basic type for native wire decode");
        return Term{.value = Literal{.value = Null{}}};
    }
}
