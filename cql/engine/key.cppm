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
        U8 bytes[2] = { U8(v >> 8), U8(v & 0xFF) };
        append_bytes(out, bytes, 2);
    }

    static void append_s64_be(DynamicArray<U8>& out, S64 v) {
        U64 bits = static_cast<U64>(v) ^ 0x8000000000000000ULL;
        U8 bytes[8] = {
            U8(bits >> 56), U8(bits >> 48), U8(bits >> 40), U8(bits >> 32),
            U8(bits >> 24), U8(bits >> 16), U8(bits >> 8),  U8(bits & 0xFF)
        };
        append_bytes(out, bytes, 8);
    }

    static void append_s32_be(DynamicArray<U8>& out, S32 v) {
        U32 bits = static_cast<U32>(v) ^ 0x80000000U;
        U8 bytes[4] = { U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF) };
        append_bytes(out, bytes, 4);
    }

    static void append_s16_be(DynamicArray<U8>& out, S16 v) {
        U16 bits = static_cast<U16>(v) ^ 0x8000U;
        U8 bytes[2] = { U8(bits >> 8), U8(bits & 0xFF) };
        append_bytes(out, bytes, 2);
    }

    static void append_f64_be(DynamicArray<U8>& out, F64 v) {
        U64 bits;
        os::memory_copy(&bits, &v, sizeof(bits));
        bits = (bits & 0x8000000000000000ULL) ? ~bits : (bits ^ 0x8000000000000000ULL);
        U8 bytes[8] = {
            U8(bits >> 56), U8(bits >> 48), U8(bits >> 40), U8(bits >> 32),
            U8(bits >> 24), U8(bits >> 16), U8(bits >> 8),  U8(bits & 0xFF)
        };
        append_bytes(out, bytes, 8);
    }

    // Escape 0x00 bytes as 0x00 0xFF, then write terminator 0x00 0x00.
    // Used for variable-length composite key components to preserve lexicographic ordering.
    static void append_escaped_terminated(DynamicArray<U8>& out, const U8* src, U16 len) {
        for (U16 i = 0; i < len; i++) {
            push_back(out, src[i]);
            if (src[i] == 0x00)
                push_back(out, U8(0xFF));
        }
        push_back(out, U8(0x00));
        push_back(out, U8(0x00));
    }

    static S64 eval_as_s64(const Evaluated& eval) {
        if (type_matches_tag<Constant>(eval.value)) return get<S64>(get<Constant>(eval.value).value);
        const ColumnValue& cv = get<ColumnValue>(eval.value);
        if (type_matches_tag<S64>(cv)) return get<S64>(cv);
        if (type_matches_tag<S32>(cv)) return S64(get<S32>(cv));
        if (type_matches_tag<S16>(cv)) return S64(get<S16>(cv));
        if (type_matches_tag<U8>(cv))  return S64(get<U8>(cv));
        assert_true(false, "unexpected ColumnValue type for integer key"); return 0;
    }
    static bool eval_as_bool(const Evaluated& eval) {
        if (type_matches_tag<Constant>(eval.value)) return get<bool>(get<Constant>(eval.value).value);
        return get<U8>(get<ColumnValue>(eval.value)) != 0;
    }
    static F64 eval_as_f64(const Evaluated& eval) {
        if (type_matches_tag<Constant>(eval.value)) return get<F64>(get<Constant>(eval.value).value);
        const ColumnValue& cv = get<ColumnValue>(eval.value);
        if (type_matches_tag<F64>(cv)) return get<F64>(cv);
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
                if (is_composite) append_u16_be(out, 8);
                append_s64_be(out, eval_as_s64(eval));
                break;
            }
            case type::Basic::int_: {
                if (is_composite) append_u16_be(out, 4);
                append_s32_be(out, static_cast<S32>(eval_as_s64(eval)));
                break;
            }
            case type::Basic::smallint: {
                if (is_composite) append_u16_be(out, 2);
                append_s16_be(out, static_cast<S16>(eval_as_s64(eval)));
                break;
            }
            case type::Basic::tinyint: {
                U8 bits = static_cast<U8>(static_cast<S8>(get<S64>(c.value))) ^ 0x80U;
                if (is_composite) append_u16_be(out, 1);
                push_back(out, bits);
                break;
            }
            case type::Basic::boolean: {
                if (is_composite) append_u16_be(out, 1);
                push_back(out, eval_as_bool(eval) ? U8(1) : U8(0));
                break;
            }
            case type::Basic::double_: {
                if (is_composite) append_u16_be(out, 8);
                append_f64_be(out, eval_as_f64(eval));
                break;
            }
            case type::Basic::float_: {
                F32 v = static_cast<F32>(get<F64>(c.value));
                U32 bits;
                os::memory_copy(&bits, &v, sizeof(bits));
                bits = (bits & 0x80000000U) ? ~bits : (bits ^ 0x80000000U);
                U8 bytes[4] = { U8(bits >> 24), U8(bits >> 16), U8(bits >> 8), U8(bits & 0xFF) };
                if (is_composite) append_u16_be(out, 4);
                append_bytes(out, bytes, 4);
                break;
            }
            case type::Basic::text:
            case type::Basic::varchar:
            case type::Basic::ascii: {
                const AutoString8& s = get<AutoString8>(c.value);
                const U8* p = reinterpret_cast<const U8*>(s.c_str);
                const U16 n = static_cast<U16>(s.length);
                if (is_composite) append_escaped_terminated(out, p, n);
                else              append_bytes(out, p, n);
                break;
            }
            case type::Basic::uuid:
            case type::Basic::timeuuid: {
                const UUID& u = get<UUID>(c.value);
                if (is_composite) append_u16_be(out, static_cast<U16>(UUID::length));
                append_bytes(out, &u.value[0], static_cast<U16>(UUID::length));
                break;
            }
            case type::Basic::blob: {
                const Blob& b = get<Blob>(c.value);
                if (is_composite) append_escaped_terminated(out, b.value.ptr, static_cast<U16>(b.value.length));
                else              append_bytes(out, b.value.ptr, static_cast<U16>(b.value.length));
                break;
            }
            case type::Basic::hex: {
                const Hex& h = get<Hex>(c.value);
                if (is_composite) append_escaped_terminated(out, h.value.ptr, static_cast<U16>(h.value.length));
                else              append_bytes(out, h.value.ptr, static_cast<U16>(h.value.length));
                break;
            }
            default:
                assert_not_implemented("key serialization for this type is not implemented");
                break;
        }
    }
}

export namespace cql::key {
    // Serialize partition key bytes from one evaluated value (single-column partition key).
    DynamicArray<U8> serialize_partition_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.partition_key_col_indices.length >= 1, "table has no partition key");
        U64 col_idx = tbl.partition_key_col_indices[0];
        const schema::Column& col = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
        DynamicArray<U8> out;
        append_component(out, val, get<type::Basic>(col.type.value), false);
        return out;
    }

    // Serialize partition key bytes from a parallel array of evaluated values.
    // partition_vals[i] corresponds to tbl.partition_key_col_indices[i].
    DynamicArray<U8> serialize_partition(const schema::Table& tbl,
                                          TArrayView<const Evaluated, U64> partition_vals) {
        assert_true(partition_vals.length == tbl.partition_key_col_indices.length,
                    "partition val count must match partition key column count");
        DynamicArray<U8> out;
        bool composite = tbl.partition_key_col_indices.length > 1;
        for (U64 i = 0; i < tbl.partition_key_col_indices.length; i++) {
            U64 col_idx = tbl.partition_key_col_indices[i];
            const schema::Column& col = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
            append_component(out, partition_vals[i], get<type::Basic>(col.type.value), composite);
        }
        return out;
    }

    // Serialize clustering key bytes from one evaluated value (single-column clustering key).
    DynamicArray<U8> serialize_clustering_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.clustering_key_col_indices.length >= 1, "table has no clustering key");
        U64 col_idx = tbl.clustering_key_col_indices[0];
        const schema::Column& col = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
        DynamicArray<U8> out;
        append_component(out, val, get<type::Basic>(col.type.value), false);
        return out;
    }

    // Serialize clustering key bytes from a parallel array of evaluated values.
    DynamicArray<U8> serialize_clustering(const schema::Table& tbl, TArrayView<const Evaluated, U64> clustering_vals) {
        assert_true(clustering_vals.length == tbl.clustering_key_col_indices.length,
                    "clustering val count must match clustering key column count");
        DynamicArray<U8> out;
        bool composite = tbl.clustering_key_col_indices.length > 1;
        for (U64 i = 0; i < tbl.clustering_key_col_indices.length; i++) {
            U64 col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            append_component(out, clustering_vals[i], get<type::Basic>(col.type.value), composite);
        }
        return out;
    }
}
