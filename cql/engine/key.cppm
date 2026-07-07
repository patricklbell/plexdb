export module cql.engine.key;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.schema;
import cql.engine.token;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;

// How keys are encoded for valid lexicographic ordering:
//
// Integers (append_s64_be, append_s32_be, etc.): XOR the sign bit, then write big-endian.
// S64 -1   → bits = 0xFFFFFFFFFFFFFFFF ^ 0x8000000000000000 = 0x7FFFFFFFFFFFFFFF
// S64  0   → bits = 0x0000000000000000 ^ 0x8000000000000000 = 0x8000000000000000
// S64  1   → bits = 0x0000000000000001 ^ 0x8000000000000000 = 0x8000000000000001
// The XOR maps [INT64_MIN, INT64_MAX] → [0, UINT64_MAX] monotonically. Then big-endian
// ensures the most significant byte is compared first.
//
// Floats: For positive values, flip the sign bit (same trick). For negative values, flip
// all bits.
//
// Text/blob: Null terminated. Length encoding would cause issues e.g. ["b"] < ["aa"]
namespace cql::key {
    static void append_bytes(DynamicArray<U8>& out, const U8* src, U16 len) {
        U64 old_len = out.length;
        resize(out, old_len + len);
        os::memory_copy(out.ptr + old_len, src, len);
    }

    static void append_u16_be(DynamicArray<U8>& out, U16 v) {
        U8 bytes[2] = {U8(v >> 8), U8(v & 0xFF)};
        append_bytes(out, bytes, 2);
    }

    static void append_s64_be(DynamicArray<U8>& out, S64 v) {
        U64 bits     = static_cast<U64>(v) ^ 0x8000000000000000ULL;
        U8  bytes[8] = {
            U8(bits >> 56), U8(bits >> 48), U8(bits >> 40), U8(bits >> 32),
            U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF)
        };
        append_bytes(out, bytes, 8);
    }

    static void append_s32_be(DynamicArray<U8>& out, S32 v) {
        U32 bits     = static_cast<U32>(v) ^ 0x80000000U;
        U8  bytes[4] = {U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF)};
        append_bytes(out, bytes, 4);
    }

    static void append_s16_be(DynamicArray<U8>& out, S16 v) {
        U16 bits     = static_cast<U16>(v) ^ 0x8000U;
        U8  bytes[2] = {U8(bits >> 8), U8(bits & 0xFF)};
        append_bytes(out, bytes, 2);
    }

    static void append_f64_be(DynamicArray<U8>& out, F64 v) {
        U64 bits;
        os::memory_copy(&bits, &v, sizeof(bits));
        bits        = (bits & 0x8000000000000000ULL) ? ~bits : (bits ^ 0x8000000000000000ULL);
        U8 bytes[8] = {
            U8(bits >> 56), U8(bits >> 48), U8(bits >> 40), U8(bits >> 32),
            U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF)
        };
        append_bytes(out, bytes, 8);
    }

    // @note DESC encoding inverts every emitted value byte so forward lex iteration produces
    // logical descending order. Length prefixes are not inverted.
    static constexpr U8 desc_mask(Sort d) {
        return d == Sort::DESC ? U8(0xFFu) : U8(0x00u);
    }
    static void xor_range_inplace(U8* p, U64 n, U8 mask) {
        if (mask == 0) {
            return;
        }
        for (U64 i = 0; i < n; i++) {
            p[i] = static_cast<U8>(p[i] ^ mask);
        }
    }
    static void xor_tail_inplace(DynamicArray<U8>& out, U64 start, U8 mask) {
        xor_range_inplace(out.ptr + start, out.length - start, mask);
    }

    // Escape 0x00 bytes as 0x00 0xFF, then write terminator 0x00 0x00.
    // Used for variable-length composite key components to preserve lexicographic ordering.
    static void append_escaped_terminated(DynamicArray<U8>& out, const U8* src, U16 len, Sort direction = Sort::ASC) {
        U64 start = out.length;
        for (U16 i = 0; i < len; i++) {
            push_back(out, src[i]);
            if (src[i] == 0x00) {
                push_back(out, U8(0xFF));
            }
        }
        push_back(out, U8(0x00));
        push_back(out, U8(0x00));
        xor_tail_inplace(out, start, desc_mask(direction));
    }

    static S64 eval_as_s64(const Evaluated& eval) {
        if (type_matches_tag<Literal>(eval.value)) {
            const auto& cv = get<Literal>(eval.value).value;
            if (type_matches_tag<S64>(cv)) {
                return get<S64>(cv);
            }
            if (type_matches_tag<S32>(cv)) {
                return S64(get<S32>(cv));
            }
            if (type_matches_tag<S16>(cv)) {
                return S64(get<S16>(cv));
            }
            if (type_matches_tag<U8>(cv)) {
                return S64(get<U8>(cv));
            }
            return 0;
        }
        const ColumnValue& cv = get<ColumnValue>(eval.value);
        if (type_matches_tag<S64>(cv)) {
            return get<S64>(cv);
        }
        if (type_matches_tag<S32>(cv)) {
            return S64(get<S32>(cv));
        }
        if (type_matches_tag<S16>(cv)) {
            return S64(get<S16>(cv));
        }
        if (type_matches_tag<U8>(cv)) {
            return S64(get<U8>(cv));
        }
        assert_true(false, "unexpected ColumnValue type for integer key");
        return 0;
    }
    static bool eval_as_bool(const Evaluated& eval) {
        if (type_matches_tag<Literal>(eval.value)) {
            return get<bool>(get<Literal>(eval.value).value);
        }
        return get<U8>(get<ColumnValue>(eval.value)) != 0;
    }
    static F64 eval_as_f64(const Evaluated& eval) {
        if (type_matches_tag<Literal>(eval.value)) {
            return get<F64>(get<Literal>(eval.value).value);
        }
        const ColumnValue& cv = get<ColumnValue>(eval.value);
        if (type_matches_tag<F64>(cv)) {
            return get<F64>(cv);
        }
        return F64(get<F32>(cv));
    }
    // Append one key component.
    // Fixed-width types in composite keys: prepend a 2-byte big-endian length (constant, so ordering is unaffected).
    // Variable-length types in composite keys: use escaped-terminated encoding to preserve ordering.
    // @note `direction == Sort::DESC` inverts every value byte (and, for variable-width
    // types, the escape and terminator bytes as well) so forward lex iteration produces
    // the value in descending logical order. The 2-byte length prefix is not inverted —
    // for fixed-width components the length is constant and inverting it would not change
    // any pair's relative order.
    static void append_component(DynamicArray<U8>& out, const Evaluated& eval, type::Basic dtype, bool is_composite, Sort direction = Sort::ASC) {
        const Literal& c           = get<Literal>(eval.value);
        U64            value_start = out.length;
        switch (dtype) {
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time: {
                if (is_composite) {
                    append_u16_be(out, 8);
                }
                value_start = out.length;
                append_s64_be(out, eval_as_s64(eval));
                break;
            }
            case type::Basic::int_:
            case type::Basic::date: {
                if (is_composite) {
                    append_u16_be(out, 4);
                }
                value_start = out.length;
                append_s32_be(out, static_cast<S32>(eval_as_s64(eval)));
                break;
            }
            case type::Basic::smallint: {
                if (is_composite) {
                    append_u16_be(out, 2);
                }
                value_start = out.length;
                append_s16_be(out, static_cast<S16>(eval_as_s64(eval)));
                break;
            }
            case type::Basic::tinyint: {
                U8 bits = static_cast<U8>(static_cast<S8>(get<S64>(c.value))) ^ 0x80U;
                if (is_composite) {
                    append_u16_be(out, 1);
                }
                value_start = out.length;
                push_back(out, bits);
                break;
            }
            case type::Basic::boolean: {
                if (is_composite) {
                    append_u16_be(out, 1);
                }
                value_start = out.length;
                push_back(out, eval_as_bool(eval) ? U8(1) : U8(0));
                break;
            }
            case type::Basic::double_: {
                if (is_composite) {
                    append_u16_be(out, 8);
                }
                value_start = out.length;
                append_f64_be(out, eval_as_f64(eval));
                break;
            }
            case type::Basic::float_: {
                F32 v = static_cast<F32>(get<F64>(c.value));
                U32 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                bits        = (bits & 0x80000000U) ? ~bits : (bits ^ 0x80000000U);
                U8 bytes[4] = {U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF)};
                if (is_composite) {
                    append_u16_be(out, 4);
                }
                value_start = out.length;
                append_bytes(out, bytes, 4);
                break;
            }
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                const AutoString8& s = get<AutoString8>(c.value);
                const U8*          p = reinterpret_cast<const U8*>(s.c_str);
                const U16          n = static_cast<U16>(s.length);
                if (is_composite) {
                    append_escaped_terminated(out, p, n, direction);
                } else {
                    append_bytes(out, p, n);
                    xor_tail_inplace(out, value_start, desc_mask(direction));
                }
                return;
            }
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                const UUID& u = get<UUID>(c.value);
                if (is_composite) {
                    append_u16_be(out, static_cast<U16>(UUID::length));
                }
                value_start = out.length;
                append_bytes(out, &u.value[0], static_cast<U16>(UUID::length));
                break;
            }
            case type::Basic::blob: {
                const Blob& b = get<Blob>(c.value);
                if (is_composite) {
                    append_escaped_terminated(out, b.value.ptr, static_cast<U16>(b.value.length), direction);
                } else {
                    append_bytes(out, b.value.ptr, static_cast<U16>(b.value.length));
                    xor_tail_inplace(out, value_start, desc_mask(direction));
                }
                return;
            }
            case type::Basic::hex: {
                const Hex& h = get<Hex>(c.value);
                if (is_composite) {
                    append_escaped_terminated(out, h.value.ptr, static_cast<U16>(h.value.length), direction);
                } else {
                    append_bytes(out, h.value.ptr, static_cast<U16>(h.value.length));
                    xor_tail_inplace(out, value_start, desc_mask(direction));
                }
                return;
            }
            default:
                assert_not_implemented("key serialization for this type is not implemented");
                return;
        }
        xor_tail_inplace(out, value_start, desc_mask(direction));
    }

    // Decode one key component from `src` starting at `*pos`. Advances *pos past the component.
    // @note when `direction == Sort::DESC` the value bytes (not the length prefix) were
    // inverted at encode time; we XOR with 0xFF before decoding to recover the original.
    static ColumnValue decode_component(const U8* src, U16 total_len, U16* pos, type::Basic dtype, bool composite, Sort direction = Sort::ASC) {
        const U8 m = (direction == Sort::DESC) ? U8(0xFFu) : U8(0x00u);
        if (composite) {
            switch (dtype) {
                // Fixed-width composite components: 2-byte length prefix + data
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time: {
                    *pos += 2;
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | (src[*pos + b] ^ m);
                    }
                    *pos += 8;
                    return ColumnValue{static_cast<S64>(bits ^ 0x8000000000000000ULL)};
                }
                case type::Basic::int_:
                case type::Basic::date: {
                    *pos += 2;
                    U32 bits = (U32(src[*pos] ^ m) << 24) | (U32(src[*pos + 1] ^ m) << 16) | (U32(src[*pos + 2] ^ m) << 8) | U32(src[*pos + 3] ^ m);
                    *pos += 4;
                    return ColumnValue{static_cast<S32>(bits ^ 0x80000000U)};
                }
                case type::Basic::smallint: {
                    *pos += 2;
                    U16 bits = (U16(src[*pos] ^ m) << 8) | U16(src[*pos + 1] ^ m);
                    *pos += 2;
                    return ColumnValue{static_cast<S16>(bits ^ 0x8000U)};
                }
                case type::Basic::tinyint: {
                    *pos += 2;
                    U8 bits = (src[*pos] ^ m) ^ 0x80U;
                    *pos += 1;
                    return ColumnValue{bits};
                }
                case type::Basic::boolean: {
                    *pos += 2;
                    U8 v = src[*pos] ^ m;
                    *pos += 1;
                    return ColumnValue{v};
                }
                case type::Basic::double_: {
                    *pos += 2;
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | (src[*pos + b] ^ m);
                    }
                    *pos += 8;
                    bits = (bits & 0x8000000000000000ULL) ? (bits ^ 0x8000000000000000ULL) : ~bits;
                    F64 v;
                    os::memory_copy(&v, &bits, 8);
                    return ColumnValue{v};
                }
                case type::Basic::float_: {
                    *pos += 2;
                    U32 bits = (U32(src[*pos] ^ m) << 24) | (U32(src[*pos + 1] ^ m) << 16) | (U32(src[*pos + 2] ^ m) << 8) | U32(src[*pos + 3] ^ m);
                    *pos += 4;
                    bits = (bits & 0x80000000U) ? (bits ^ 0x80000000U) : ~bits;
                    F32 v;
                    os::memory_copy(&v, &bits, 4);
                    return ColumnValue{v};
                }
                case type::Basic::uuid:
                case type::Basic::timeuuid: {
                    *pos += 2;
                    UUID u;
                    for (U64 i = 0; i < UUID::length; i++) {
                        u.value[i] = static_cast<U8>(src[*pos + i] ^ m);
                    }
                    *pos += static_cast<U16>(UUID::length);
                    return ColumnValue{u};
                }
                // Variable-width composite components: escaped-terminated
                case type::Basic::text:
                case type::Basic::varchar:
                case type::Basic::ascii: {
                    DynamicArray<U8> buf;
                    while (*pos + 1 < total_len) {
                        U8 c = static_cast<U8>(src[*pos] ^ m);
                        *pos += 1;
                        if (c == 0x00) {
                            U8 next = static_cast<U8>(src[*pos] ^ m);
                            if (next == 0xFF) {
                                *pos += 1;
                                push_back(buf, U8(0x00));
                            } else {
                                *pos += 1;
                                break;
                            }
                        } else {
                            push_back(buf, c);
                        }
                    }
                    AutoString8 s{buf.length};
                    os::memory_copy(s.c_str, buf.ptr, buf.length);
                    return ColumnValue{move(s)};
                }
                case type::Basic::blob:
                case type::Basic::hex: {
                    DynamicArray<U8> buf;
                    while (*pos + 1 < total_len) {
                        U8 c = static_cast<U8>(src[*pos] ^ m);
                        *pos += 1;
                        if (c == 0x00) {
                            U8 next = static_cast<U8>(src[*pos] ^ m);
                            if (next == 0xFF) {
                                *pos += 1;
                                push_back(buf, U8(0x00));
                            } else {
                                *pos += 1;
                                break;
                            }
                        } else {
                            push_back(buf, c);
                        }
                    }
                    Blob b;
                    b.value = move(buf);
                    return ColumnValue{move(b)};
                }
                default:
                    assert_not_implemented("key deserialization not implemented for this type");
                    return ColumnValue{Null{}};
            }
        } else {
            switch (dtype) {
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time: {
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | (src[*pos + b] ^ m);
                    }
                    *pos += 8;
                    return ColumnValue{static_cast<S64>(bits ^ 0x8000000000000000ULL)};
                }
                case type::Basic::int_:
                case type::Basic::date: {
                    U32 bits = (U32(src[*pos] ^ m) << 24) | (U32(src[*pos + 1] ^ m) << 16) | (U32(src[*pos + 2] ^ m) << 8) | U32(src[*pos + 3] ^ m);
                    *pos += 4;
                    return ColumnValue{static_cast<S32>(bits ^ 0x80000000U)};
                }
                case type::Basic::smallint: {
                    U16 bits = (U16(src[*pos] ^ m) << 8) | U16(src[*pos + 1] ^ m);
                    *pos += 2;
                    return ColumnValue{static_cast<S16>(bits ^ 0x8000U)};
                }
                case type::Basic::tinyint: {
                    U8 bits = (src[*pos] ^ m) ^ 0x80U;
                    *pos += 1;
                    return ColumnValue{bits};
                }
                case type::Basic::boolean: {
                    U8 v = src[*pos] ^ m;
                    *pos += 1;
                    return ColumnValue{v};
                }
                case type::Basic::double_: {
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | (src[*pos + b] ^ m);
                    }
                    *pos += 8;
                    bits = (bits & 0x8000000000000000ULL) ? (bits ^ 0x8000000000000000ULL) : ~bits;
                    F64 v;
                    os::memory_copy(&v, &bits, 8);
                    return ColumnValue{v};
                }
                case type::Basic::float_: {
                    U32 bits = (U32(src[*pos] ^ m) << 24) | (U32(src[*pos + 1] ^ m) << 16) | (U32(src[*pos + 2] ^ m) << 8) | U32(src[*pos + 3] ^ m);
                    *pos += 4;
                    bits = (bits & 0x80000000U) ? (bits ^ 0x80000000U) : ~bits;
                    F32 v;
                    os::memory_copy(&v, &bits, 4);
                    return ColumnValue{v};
                }
                case type::Basic::uuid:
                case type::Basic::timeuuid: {
                    UUID u;
                    for (U64 i = 0; i < UUID::length; i++) {
                        u.value[i] = static_cast<U8>(src[*pos + i] ^ m);
                    }
                    *pos += static_cast<U16>(UUID::length);
                    return ColumnValue{u};
                }
                case type::Basic::text:
                case type::Basic::varchar:
                case type::Basic::ascii: {
                    U16         slen = static_cast<U16>(total_len - *pos);
                    AutoString8 s{slen};
                    os::memory_copy(s.c_str, src + *pos, slen);
                    xor_range_inplace(reinterpret_cast<U8*>(s.c_str), slen, m);
                    *pos += slen;
                    return ColumnValue{move(s)};
                }
                case type::Basic::blob:
                case type::Basic::hex: {
                    U16  slen = static_cast<U16>(total_len - *pos);
                    Blob b;
                    resize(b.value, U64(slen));
                    os::memory_copy(b.value.ptr, src + *pos, slen);
                    xor_range_inplace(b.value.ptr, slen, m);
                    *pos += slen;
                    return ColumnValue{move(b)};
                }
                default:
                    assert_not_implemented("key deserialization not implemented for this type");
                    return ColumnValue{Null{}};
            }
        }
    }
}

namespace cql::key {
    // Returns true for types whose encoding length is not determined by dtype alone.
    static bool is_variable_length_basic(type::Basic dtype) {
        return dtype == type::Basic::text || dtype == type::Basic::varchar || dtype == type::Basic::ascii || dtype == type::Basic::blob || dtype == type::Basic::hex;
    }

    // @note append_component encodes from Literal, so widen ColumnValue's narrow
    // numeric representations (S16, S32, S8, F32, U8-as-bool) into the matching Literal types.
    static Evaluated cv_to_const_eval(const ColumnValue& cv, type::Basic dtype) {
        Literal c{};
        switch (dtype) {
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time:
                c.value = get<S64>(cv);
                break;
            case type::Basic::int_:
            case type::Basic::date:
                c.value = S64(get<S32>(cv));
                break;
            case type::Basic::smallint:
                c.value = S64(get<S16>(cv));
                break;
            // tinyint: ColumnValue holds raw U8 bit pattern (S8 reinterpreted). Literal needs S64.
            case type::Basic::tinyint:
                c.value = S64(static_cast<S8>(get<U8>(cv)));
                break;
            case type::Basic::boolean:
                c.value = bool(get<U8>(cv) != 0);
                break;
            case type::Basic::double_:
                c.value = get<F64>(cv);
                break;
            // float: ColumnValue holds F32. Literal needs F64; append_component casts back.
            case type::Basic::float_:
                c.value = F64(get<F32>(cv));
                break;
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii:
                c.value = get<AutoString8>(cv);
                break;
            case type::Basic::uuid:
            case type::Basic::timeuuid:
                c.value = get<UUID>(cv);
                break;
            case type::Basic::blob:
                c.value = get<Blob>(cv);
                break;
            default:
                assert_not_implemented("index key encoding for this type is not implemented");
                break;
        }
        Evaluated e{};
        e.value = c;
        return e;
    }

    // Partition keys are stored in the BTree prefixed by an 8-byte big-endian
    // token (sign-bit XOR'd for lex order). Byte-wise compare over the encoding
    // matches signed compare on the token, with the raw partition bytes acting
    // as a deterministic tiebreaker when two keys hash to the same token.
    static constexpr U64 PARTITION_TOKEN_PREFIX_BYTES = 8;

    static void append_be(DynamicArray<U8>& out, U64 bits, U64 nbytes) {
        for (U64 i = nbytes; i-- > 0;) {
            push_back(out, U8((bits >> (i * 8)) & 0xFFu));
        }
    }

    // Cassandra wire-format encoding of one partition-key value, used only for
    // token computation (never written to disk). Composite components carry a
    // U16 BE length prefix and a 0x00 separator; single components are raw bytes.
    static void append_wire_partition_component(DynamicArray<U8>& out, const Evaluated& eval, type::Basic dtype, bool is_composite) {
        DynamicArray<U8> val;
        switch (dtype) {
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter:
            case type::Basic::time:
                append_be(val, static_cast<U64>(eval_as_s64(eval)), 8);
                break;
            case type::Basic::int_:
            case type::Basic::date:
                append_be(val, U64(U32(static_cast<S32>(eval_as_s64(eval)))), 4);
                break;
            case type::Basic::smallint:
                append_be(val, U64(U16(static_cast<S16>(eval_as_s64(eval)))), 2);
                break;
            case type::Basic::tinyint:
                push_back(val, static_cast<U8>(static_cast<S8>(get<S64>(get<Literal>(eval.value).value))));
                break;
            case type::Basic::boolean:
                push_back(val, eval_as_bool(eval) ? U8(1) : U8(0));
                break;
            case type::Basic::double_: {
                F64 v = eval_as_f64(eval);
                U64 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                append_be(val, bits, 8);
                break;
            }
            case type::Basic::float_: {
                F32 v = static_cast<F32>(get<F64>(get<Literal>(eval.value).value));
                U32 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                append_be(val, U64(bits), 4);
                break;
            }
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                const AutoString8& s = get<AutoString8>(get<Literal>(eval.value).value);
                append_bytes(val, reinterpret_cast<const U8*>(s.c_str), static_cast<U16>(s.length));
                break;
            }
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                const UUID& u = get<UUID>(get<Literal>(eval.value).value);
                append_bytes(val, &u.value[0], static_cast<U16>(UUID::length));
                break;
            }
            case type::Basic::blob: {
                const Blob& b = get<Blob>(get<Literal>(eval.value).value);
                append_bytes(val, b.value.ptr, static_cast<U16>(b.value.length));
                break;
            }
            case type::Basic::hex: {
                const Hex& h = get<Hex>(get<Literal>(eval.value).value);
                append_bytes(val, h.value.ptr, static_cast<U16>(h.value.length));
                break;
            }
            default:
                assert_not_implemented("wire encoding for partition key type is not implemented");
                return;
        }
        if (is_composite) {
            append_u16_be(out, static_cast<U16>(val.length));
            append_bytes(out, val.ptr, static_cast<U16>(val.length));
            push_back(out, U8(0));
        } else {
            append_bytes(out, val.ptr, static_cast<U16>(val.length));
        }
    }

    // @note token bytes are written into a separate buffer first, then the lex
    // partition bytes are appended. Writing in this order avoids an in-place shift
    // of the entire lex tail.
    static void append_token_prefix(DynamicArray<U8>& out, const DynamicArray<U8>& wire) {
        U8 buf[PARTITION_TOKEN_PREFIX_BYTES];
        token::encode_token_be(buf, token::murmur3_token(wire.ptr, wire.length));
        append_bytes(out, buf, static_cast<U16>(PARTITION_TOKEN_PREFIX_BYTES));
    }
}

export namespace cql::key {
    // Serialize partition key bytes from one evaluated value (single-column partition key).
    DynamicArray<U8> serialize_partition_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.partition_key_col_indices.length >= 1, "table has no partition key");
        U64                   col_idx = tbl.partition_key_col_indices[0];
        const schema::Column& col     = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
        type::Basic      dtype = get<type::Basic>(col.type.value);
        DynamicArray<U8> wire;
        append_wire_partition_component(wire, val, dtype, false);
        DynamicArray<U8> out;
        append_token_prefix(out, wire);
        append_component(out, val, dtype, false);
        return out;
    }

    // Serialize partition key bytes from a parallel array of evaluated values.
    // partition_vals[i] corresponds to tbl.partition_key_col_indices[i].
    DynamicArray<U8> serialize_partition(const schema::Table& tbl, TArrayView<const Evaluated, U64> partition_vals) {
        assert_true(partition_vals.length == tbl.partition_key_col_indices.length, "partition val count must match partition key column count");
        bool             composite = tbl.partition_key_col_indices.length > 1;
        DynamicArray<U8> wire;
        for (U64 i = 0; i < tbl.partition_key_col_indices.length; i++) {
            U64                   col_idx = tbl.partition_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
            append_wire_partition_component(wire, partition_vals[i], get<type::Basic>(col.type.value), composite);
        }
        DynamicArray<U8> out;
        append_token_prefix(out, wire);
        for (U64 i = 0; i < tbl.partition_key_col_indices.length; i++) {
            U64         col_idx = tbl.partition_key_col_indices[i];
            type::Basic dtype   = get<type::Basic>(tbl.cols[col_idx].type.value);
            append_component(out, partition_vals[i], dtype, composite);
        }
        return out;
    }

    // Serialize the first clustering key column as a range-bound prefix.
    // Uses composite encoding when the table has multiple CK columns so the bound
    // bytes match the composite key format stored in the BTree.
    DynamicArray<U8> serialize_clustering_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.clustering_key_col_indices.length >= 1, "table has no clustering key");
        U64                   col_idx = tbl.clustering_key_col_indices[0];
        const schema::Column& col     = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
        bool             is_composite = tbl.clustering_key_col_indices.length > 1;
        DynamicArray<U8> out;
        append_component(out, val, get<type::Basic>(col.type.value), is_composite, col.clustering_order);
        return out;
    }

    // Serialize clustering key bytes from a parallel array of evaluated values.
    DynamicArray<U8> serialize_clustering(const schema::Table& tbl, TArrayView<const Evaluated, U64> clustering_vals) {
        assert_true(clustering_vals.length == tbl.clustering_key_col_indices.length, "clustering val count must match clustering key column count");
        DynamicArray<U8> out;
        bool             composite = tbl.clustering_key_col_indices.length > 1;
        for (U64 i = 0; i < tbl.clustering_key_col_indices.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            append_component(out, clustering_vals[i], get<type::Basic>(col.type.value), composite, col.clustering_order);
        }
        return out;
    }

    DynamicArray<U8> serialize_clustering_prefix(const schema::Table& tbl, TArrayView<const Evaluated, U64> vals) {
        assert_true(vals.length <= tbl.clustering_key_col_indices.length, "prefix length exceeds clustering key column count");
        DynamicArray<U8> out;
        bool             composite = tbl.clustering_key_col_indices.length > 1;
        for (U64 i = 0; i < vals.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            append_component(out, vals[i], get<type::Basic>(col.type.value), composite, col.clustering_order);
        }
        return out;
    }

    // @note variable-length values must be escape-terminated so that the prefix boundary
    // is unambiguous even when the value contains zero bytes; fixed-width values do not need it.
    DynamicArray<U8> make_index_prefix(const Evaluated& val, type::Basic dtype) {
        DynamicArray<U8> out;
        append_component(out, val, dtype, is_variable_length_basic(dtype));
        return out;
    }

    DynamicArray<U8> make_index_prefix_from_cv(const ColumnValue& cv, type::Basic dtype) {
        return make_index_prefix(cv_to_const_eval(cv, dtype), dtype);
    }

    // @note composite form: prepends a 2-byte length for fixed-width and uses
    // escape-terminated encoding for variable-length, so the boundary is
    // recoverable when concatenated with further components (e.g. entries(map)
    // indexes whose prefix is key ++ value).
    DynamicArray<U8> make_index_prefix_composite_from_cv(const ColumnValue& cv, type::Basic dtype) {
        DynamicArray<U8> out;
        append_component(out, cv_to_const_eval(cv, dtype), dtype, true);
        return out;
    }

    // Entries-index lookup prefix for `WHERE m[k] = v`: composite-encoded key
    // followed by the raw value form (matches what the writer stamps per entry).
    DynamicArray<U8> make_index_prefix_entries(const Evaluated& key_val, type::Basic key_dtype, const Evaluated& val_val, type::Basic val_dtype) {
        DynamicArray<U8> out;
        append_component(out, key_val, key_dtype, true);
        append_component(out, val_val, val_dtype, is_variable_length_basic(val_dtype));
        return out;
    }

    U16 index_prefix_len(type::Basic dtype, const U8* key_ptr) {
        if (!is_variable_length_basic(dtype)) {
            switch (dtype) {
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time:
                case type::Basic::double_:
                    return 8;
                case type::Basic::int_:
                case type::Basic::date:
                case type::Basic::float_:
                    return 4;
                case type::Basic::smallint:
                    return 2;
                case type::Basic::tinyint:
                case type::Basic::boolean:
                    return 1;
                case type::Basic::uuid:
                case type::Basic::timeuuid:
                    return static_cast<U16>(UUID::length);
                default:
                    return 0;
            }
        }
        // @note escape format: 0x00 0xFF = literal NUL, 0x00 0x00 = terminator.
        U16 i = 0;
        while (true) {
            if (key_ptr[i] == 0x00) {
                i += 2;
                if (key_ptr[i - 1] != 0xFF) {
                    break;
                }
            } else {
                i += 1;
            }
        }
        return i;
    }

    // @note inverse of make_index_prefix_composite_from_cv: fixed-width types are prefixed
    // with a U16-BE length; variable-length types use the same escape-terminated scheme.
    U16 index_prefix_len_composite(type::Basic dtype, const U8* key_ptr) {
        if (!is_variable_length_basic(dtype)) {
            U16 stored = static_cast<U16>((U16(key_ptr[0]) << 8) | U16(key_ptr[1]));
            return static_cast<U16>(2 + stored);
        }
        return index_prefix_len(dtype, key_ptr);
    }

    // @note index key layout: col_prefix || [U16 pk_len] || pk_bytes || ck_bytes
    DynamicArray<U8> make_full_index_key(const DynamicArray<U8>& prefix, TArrayView<const U8, U16> pk_bytes, TArrayView<const U8, U16> ck_bytes) {
        DynamicArray<U8> out;
        append_bytes(out, prefix.ptr, static_cast<U16>(prefix.length));
        append_u16_be(out, pk_bytes.length);
        append_bytes(out, pk_bytes.ptr, pk_bytes.length);
        if (ck_bytes.length > 0) {
            append_bytes(out, ck_bytes.ptr, ck_bytes.length);
        }
        return out;
    }

    // Deserialize partition key bytes back into Values.
    // Returns one ColumnValue per tbl.partition_key_col_indices entry.
    // @note skips the leading PARTITION_TOKEN_PREFIX_BYTES that precede the lex components.
    DynamicArray<ColumnValue> deserialize_partition(const schema::Table& tbl, TArrayView<const U8, U16> key_bytes) {
        DynamicArray<ColumnValue> out;
        bool                      composite = tbl.partition_key_col_indices.length > 1;
        U16                       pos       = static_cast<U16>(PARTITION_TOKEN_PREFIX_BYTES);
        for (U64 i = 0; i < tbl.partition_key_col_indices.length; i++) {
            U64                   col_idx = tbl.partition_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            type::Basic           dtype   = get<type::Basic>(col.type.value);
            push_back(out, decode_component(key_bytes.ptr, key_bytes.length, &pos, dtype, composite));
        }
        return out;
    }

    // Extract the token from the leading PARTITION_TOKEN_PREFIX_BYTES of an
    // encoded partition key.
    S64 partition_token_from_key_bytes(TArrayView<const U8, U16> key_bytes) {
        assert_true(key_bytes.length >= PARTITION_TOKEN_PREFIX_BYTES, "partition key is missing its token prefix");
        return token::decode_token_be(key_bytes.ptr);
    }

    // Compute the Murmur3 partition token from a row's column values, hashing
    // the Cassandra wire-format bytes directly without building the full
    // lex-encoded key.
    S64 compute_partition_token(const schema::Table& tbl, TArrayView<const ColumnValue, U64> row_values) {
        bool             composite = tbl.partition_key_col_indices.length > 1;
        DynamicArray<U8> wire;
        for (U64 pk_ci : tbl.partition_key_col_indices) {
            assert_true(pk_ci < row_values.length, "partition column index out of range");
            type::Basic dtype = get<type::Basic>(tbl.cols[pk_ci].type.value);
            append_wire_partition_component(wire, cv_to_const_eval(row_values[pk_ci], dtype), dtype, composite);
        }
        return token::murmur3_token(wire.ptr, wire.length);
    }

    // Same token computation, but taking already-evaluated argument values (the
    // shape produced by the term registry when token(...) appears in a term
    // context). evals[i] is encoded as the type of partition_key_col_indices[i].
    S64 compute_partition_token_from_evals(const schema::Table& tbl, TArrayView<const Evaluated, U64> evals) {
        assert_true(evals.length == tbl.partition_key_col_indices.length, "token(...) arity must match partition key column count");
        bool             composite = tbl.partition_key_col_indices.length > 1;
        DynamicArray<U8> wire;
        for (U64 i = 0; i < evals.length; i++) {
            type::Basic dtype = get<type::Basic>(tbl.cols[tbl.partition_key_col_indices[i]].type.value);
            append_wire_partition_component(wire, evals[i], dtype, composite);
        }
        return token::murmur3_token(wire.ptr, wire.length);
    }

    // @note partial CK prefixes are legal; trailing positions with no bytes left are simply absent.
    // The first component is always decoded so a single-column non-composite CK with an empty
    // blob/text value (0-byte key) round-trips back to an empty value rather than absent.
    DynamicArray<ColumnValue> deserialize_clustering(const schema::Table& tbl, TArrayView<const U8, U16> key_bytes) {
        DynamicArray<ColumnValue> out;
        bool                      composite = tbl.clustering_key_col_indices.length > 1;
        U16                       pos       = 0;
        for (U64 i = 0; i < tbl.clustering_key_col_indices.length; i++) {
            if (i > 0 && pos >= key_bytes.length) {
                break;
            }
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            type::Basic           dtype   = get<type::Basic>(col.type.value);
            push_back(out, decode_component(key_bytes.ptr, key_bytes.length, &pos, dtype, composite, col.clustering_order));
        }
        return out;
    }
}
