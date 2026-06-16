export module cql.engine.key;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.schema;
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
            U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF)};
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
            U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF)};
        append_bytes(out, bytes, 8);
    }

    // Escape 0x00 bytes as 0x00 0xFF, then write terminator 0x00 0x00.
    // Used for variable-length composite key components to preserve lexicographic ordering.
    static void append_escaped_terminated(DynamicArray<U8>& out, const U8* src, U16 len) {
        for (U16 i = 0; i < len; i++) {
            push_back(out, src[i]);
            if (src[i] == 0x00) {
                push_back(out, U8(0xFF));
            }
        }
        push_back(out, U8(0x00));
        push_back(out, U8(0x00));
    }

    static S64 eval_as_s64(const Evaluated& eval) {
        if (type_matches_tag<Constant>(eval.value)) {
            const auto& cv = get<Constant>(eval.value).value;
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
        if (type_matches_tag<Constant>(eval.value)) {
            return get<bool>(get<Constant>(eval.value).value);
        }
        return get<U8>(get<ColumnValue>(eval.value)) != 0;
    }
    static F64 eval_as_f64(const Evaluated& eval) {
        if (type_matches_tag<Constant>(eval.value)) {
            return get<F64>(get<Constant>(eval.value).value);
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
    static void append_component(DynamicArray<U8>& out, const Evaluated& eval,
                                 type::Basic dtype, bool is_composite) {
        const Constant& c = get<Constant>(eval.value);
        switch (dtype) {
            case type::Basic::bigint:
            case type::Basic::timestamp:
            case type::Basic::counter: {
                if (is_composite) {
                    append_u16_be(out, 8);
                }
                append_s64_be(out, eval_as_s64(eval));
                break;
            }
            case type::Basic::int_: {
                if (is_composite) {
                    append_u16_be(out, 4);
                }
                append_s32_be(out, static_cast<S32>(eval_as_s64(eval)));
                break;
            }
            case type::Basic::smallint: {
                if (is_composite) {
                    append_u16_be(out, 2);
                }
                append_s16_be(out, static_cast<S16>(eval_as_s64(eval)));
                break;
            }
            case type::Basic::tinyint: {
                U8 bits = static_cast<U8>(static_cast<S8>(get<S64>(c.value))) ^ 0x80U;
                if (is_composite) {
                    append_u16_be(out, 1);
                }
                push_back(out, bits);
                break;
            }
            case type::Basic::boolean: {
                if (is_composite) {
                    append_u16_be(out, 1);
                }
                push_back(out, eval_as_bool(eval) ? U8(1) : U8(0));
                break;
            }
            case type::Basic::double_: {
                if (is_composite) {
                    append_u16_be(out, 8);
                }
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
                    append_escaped_terminated(out, p, n);
                } else {
                    append_bytes(out, p, n);
                }
                break;
            }
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                const UUID& u = get<UUID>(c.value);
                if (is_composite) {
                    append_u16_be(out, static_cast<U16>(UUID::length));
                }
                append_bytes(out, &u.value[0], static_cast<U16>(UUID::length));
                break;
            }
            case type::Basic::blob: {
                const Blob& b = get<Blob>(c.value);
                if (is_composite) {
                    append_escaped_terminated(out, b.value.ptr, static_cast<U16>(b.value.length));
                } else {
                    append_bytes(out, b.value.ptr, static_cast<U16>(b.value.length));
                }
                break;
            }
            case type::Basic::hex: {
                const Hex& h = get<Hex>(c.value);
                if (is_composite) {
                    append_escaped_terminated(out, h.value.ptr, static_cast<U16>(h.value.length));
                } else {
                    append_bytes(out, h.value.ptr, static_cast<U16>(h.value.length));
                }
                break;
            }
            default:
                assert_not_implemented("key serialization for this type is not implemented");
                break;
        }
    }

    // Decode one key component from `src` starting at `*pos`. Advances *pos past the component.
    static ColumnValue decode_component(const U8* src, U16 total_len, U16* pos,
                                        type::Basic dtype, bool composite) {
        if (composite) {
            switch (dtype) {
                // Fixed-width composite components: 2-byte length prefix + data
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter: {
                    *pos += 2;
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | src[*pos + b];
                    }
                    *pos += 8;
                    return ColumnValue{static_cast<S64>(bits ^ 0x8000000000000000ULL)};
                }
                case type::Basic::int_: {
                    *pos += 2;
                    U32 bits = (U32(src[*pos]) << 24) | (U32(src[*pos + 1]) << 16) |
                               (U32(src[*pos + 2]) << 8) | U32(src[*pos + 3]);
                    *pos += 4;
                    return ColumnValue{static_cast<S32>(bits ^ 0x80000000U)};
                }
                case type::Basic::smallint: {
                    *pos += 2;
                    U16 bits = (U16(src[*pos]) << 8) | U16(src[*pos + 1]);
                    *pos += 2;
                    return ColumnValue{static_cast<S16>(bits ^ 0x8000U)};
                }
                case type::Basic::tinyint: {
                    *pos += 2;
                    U8 bits = src[*pos] ^ 0x80U;
                    *pos += 1;
                    return ColumnValue{bits};
                }
                case type::Basic::boolean: {
                    *pos += 2;
                    U8 v = src[*pos];
                    *pos += 1;
                    return ColumnValue{v};
                }
                case type::Basic::double_: {
                    *pos += 2;
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | src[*pos + b];
                    }
                    *pos += 8;
                    bits = (bits & 0x8000000000000000ULL) ? (bits ^ 0x8000000000000000ULL) : ~bits;
                    F64 v;
                    os::memory_copy(&v, &bits, 8);
                    return ColumnValue{v};
                }
                case type::Basic::float_: {
                    *pos += 2;
                    U32 bits = (U32(src[*pos]) << 24) | (U32(src[*pos + 1]) << 16) |
                               (U32(src[*pos + 2]) << 8) | U32(src[*pos + 3]);
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
                    os::memory_copy(&u.value[0], src + *pos, UUID::length);
                    *pos += static_cast<U16>(UUID::length);
                    return ColumnValue{u};
                }
                // Variable-width composite components: escaped-terminated
                case type::Basic::text:
                case type::Basic::varchar:
                case type::Basic::ascii: {
                    DynamicArray<U8> buf;
                    while (*pos + 1 < total_len) {
                        U8 c = src[*pos];
                        *pos += 1;
                        if (c == 0x00) {
                            if (src[*pos] == 0xFF) {
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
                default:
                    assert_not_implemented("key deserialization not implemented for this type");
                    return ColumnValue{Null{}};
            }
        } else {
            switch (dtype) {
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter: {
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | src[*pos + b];
                    }
                    *pos += 8;
                    return ColumnValue{static_cast<S64>(bits ^ 0x8000000000000000ULL)};
                }
                case type::Basic::int_: {
                    U32 bits = (U32(src[*pos]) << 24) | (U32(src[*pos + 1]) << 16) |
                               (U32(src[*pos + 2]) << 8) | U32(src[*pos + 3]);
                    *pos += 4;
                    return ColumnValue{static_cast<S32>(bits ^ 0x80000000U)};
                }
                case type::Basic::smallint: {
                    U16 bits = (U16(src[*pos]) << 8) | U16(src[*pos + 1]);
                    *pos += 2;
                    return ColumnValue{static_cast<S16>(bits ^ 0x8000U)};
                }
                case type::Basic::tinyint: {
                    U8 bits = src[*pos] ^ 0x80U;
                    *pos += 1;
                    return ColumnValue{bits};
                }
                case type::Basic::boolean: {
                    U8 v = src[*pos];
                    *pos += 1;
                    return ColumnValue{v};
                }
                case type::Basic::double_: {
                    U64 bits = 0;
                    for (int b = 0; b < 8; b++) {
                        bits = (bits << 8) | src[*pos + b];
                    }
                    *pos += 8;
                    bits = (bits & 0x8000000000000000ULL) ? (bits ^ 0x8000000000000000ULL) : ~bits;
                    F64 v;
                    os::memory_copy(&v, &bits, 8);
                    return ColumnValue{v};
                }
                case type::Basic::float_: {
                    U32 bits = (U32(src[*pos]) << 24) | (U32(src[*pos + 1]) << 16) |
                               (U32(src[*pos + 2]) << 8) | U32(src[*pos + 3]);
                    *pos += 4;
                    bits = (bits & 0x80000000U) ? (bits ^ 0x80000000U) : ~bits;
                    F32 v;
                    os::memory_copy(&v, &bits, 4);
                    return ColumnValue{v};
                }
                case type::Basic::uuid:
                case type::Basic::timeuuid: {
                    UUID u;
                    os::memory_copy(&u.value[0], src + *pos, UUID::length);
                    *pos += static_cast<U16>(UUID::length);
                    return ColumnValue{u};
                }
                case type::Basic::text:
                case type::Basic::varchar:
                case type::Basic::ascii: {
                    U16         slen = static_cast<U16>(total_len - *pos);
                    AutoString8 s{slen};
                    os::memory_copy(s.c_str, src + *pos, slen);
                    *pos += slen;
                    return ColumnValue{move(s)};
                }
                default:
                    assert_not_implemented("key deserialization not implemented for this type");
                    return ColumnValue{Null{}};
            }
        }
    }
}

export namespace cql::key {
    // Serialize partition key bytes from one evaluated value (single-column partition key).
    DynamicArray<U8> serialize_partition_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.partition_key_col_indices.length >= 1, "table has no partition key");
        U64                   col_idx = tbl.partition_key_col_indices[0];
        const schema::Column& col     = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
        DynamicArray<U8> out;
        append_component(out, val, get<type::Basic>(col.type.value), false);
        return out;
    }

    // Serialize partition key bytes from a parallel array of evaluated values.
    // partition_vals[i] corresponds to tbl.partition_key_col_indices[i].
    DynamicArray<U8> serialize_partition(const schema::Table&             tbl,
                                         TArrayView<const Evaluated, U64> partition_vals) {
        assert_true(partition_vals.length == tbl.partition_key_col_indices.length,
                    "partition val count must match partition key column count");
        DynamicArray<U8> out;
        bool             composite = tbl.partition_key_col_indices.length > 1;
        for (U64 i = 0; i < tbl.partition_key_col_indices.length; i++) {
            U64                   col_idx = tbl.partition_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
            append_component(out, partition_vals[i], get<type::Basic>(col.type.value), composite);
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
        append_component(out, val, get<type::Basic>(col.type.value), is_composite);
        return out;
    }

    // Serialize clustering key bytes from a parallel array of evaluated values.
    DynamicArray<U8> serialize_clustering(const schema::Table& tbl, TArrayView<const Evaluated, U64> clustering_vals) {
        assert_true(clustering_vals.length == tbl.clustering_key_col_indices.length,
                    "clustering val count must match clustering key column count");
        DynamicArray<U8> out;
        bool             composite = tbl.clustering_key_col_indices.length > 1;
        for (U64 i = 0; i < tbl.clustering_key_col_indices.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            append_component(out, clustering_vals[i], get<type::Basic>(col.type.value), composite);
        }
        return out;
    }

    DynamicArray<U8> serialize_clustering_prefix(const schema::Table& tbl, TArrayView<const Evaluated, U64> vals) {
        assert_true(vals.length <= tbl.clustering_key_col_indices.length,
                    "prefix length exceeds clustering key column count");
        DynamicArray<U8> out;
        bool             composite = tbl.clustering_key_col_indices.length > 1;
        for (U64 i = 0; i < vals.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            append_component(out, vals[i], get<type::Basic>(col.type.value), composite);
        }
        return out;
    }

    // Deserialize partition key bytes back into ColumnValues.
    // Returns one ColumnValue per tbl.partition_key_col_indices entry.
    DynamicArray<ColumnValue> deserialize_partition(const schema::Table& tbl, TArrayView<const U8, U16> key_bytes) {
        DynamicArray<ColumnValue> out;
        bool                      composite = tbl.partition_key_col_indices.length > 1;
        U16                       pos       = 0;
        for (U64 i = 0; i < tbl.partition_key_col_indices.length; i++) {
            U64                   col_idx = tbl.partition_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            type::Basic           dtype   = get<type::Basic>(col.type.value);
            push_back(out, decode_component(key_bytes.ptr, key_bytes.length, &pos, dtype, composite));
        }
        return out;
    }
}
