module;
#include <coroutine>

module cql.engine.io.codec;
import cql.engine.io.types;
import cql.engine.io.codec.wire;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

namespace cql::io {
    // ========================================================================
    // row metadata
    // ========================================================================
    void write_row_metadata(Writer w, const RowMetadata& m) {
        w(reinterpret_cast<const U8*>(&m.flags), sizeof(m.flags));
        w(reinterpret_cast<const U8*>(&m.expiry_unix_ms), sizeof(m.expiry_unix_ms));
    }

    coroutine::Task<RowMetadata> read_row_metadata(Reader r) {
        RowMetadata m;
        co_await r(reinterpret_cast<U8*>(&m.flags), sizeof(m.flags));
        co_await r(reinterpret_cast<U8*>(&m.expiry_unix_ms), sizeof(m.expiry_unix_ms));
        co_return m;
    }

    void write_cell_metadata(Writer w, const CellMetadata& m) {
        w(reinterpret_cast<const U8*>(&m.flags), sizeof(m.flags));
        if (cell_has_ttl(m)) {
            w(reinterpret_cast<const U8*>(&m.expiry_unix_ms), sizeof(m.expiry_unix_ms));
        }
        if (cell_has_writetime(m)) {
            w(reinterpret_cast<const U8*>(&m.writetime_us), sizeof(m.writetime_us));
        }
    }

    coroutine::Task<CellMetadata> read_cell_metadata(Reader r) {
        CellMetadata m;
        co_await r(reinterpret_cast<U8*>(&m.flags), sizeof(m.flags));
        if (cell_has_ttl(m)) {
            co_await r(reinterpret_cast<U8*>(&m.expiry_unix_ms), sizeof(m.expiry_unix_ms));
        }
        if (cell_has_writetime(m)) {
            co_await r(reinterpret_cast<U8*>(&m.writetime_us), sizeof(m.writetime_us));
        }
        co_return m;
    }

    coroutine::Task<void> skip_cell_metadata(Reader r, U8 flags) {
        U64 to_skip = 0;
        if ((flags & CELL_FLAG_HAS_TTL) != 0) {
            to_skip += sizeof(S64);
        }
        if ((flags & CELL_FLAG_HAS_WRITETIME) != 0) {
            to_skip += sizeof(S64);
        }
        if (to_skip > 0) {
            co_await r(nullptr, to_skip);
        }
    }

    void write_mask_bits(Writer w, ColumnActiveChecker is_set, U64 column_count) {
        U64 mask = 0;
        for (U64 idx = 0; idx < column_count; idx++) {
            mask |= (static_cast<U64>(is_set(idx)) << (idx % MASK_BIT_COUNT));
            if (idx % MASK_BIT_COUNT == MASK_BIT_COUNT - 1) {
                w(reinterpret_cast<const U8*>(&mask), sizeof(mask));
                mask = 0;
            }
        }
        if (column_count % MASK_BIT_COUNT != 0) {
            w(reinterpret_cast<const U8*>(&mask), sizeof(mask));
        }
    }

    // read_column_value(Reader, type::Basic) is generated in generated/io_codec_wire.cpp.

    // ========================================================================
    // masked-payload helpers (shared by Tuple and UDT value serialisation)
    //
    // Shared on-disk form: a ceil(count/64)-word null bitmask, followed by per-field payloads
    // for fields with bit set. Tuple uses the type's immutable arity as the count; UDT writes
    // a leading U64 count so readers can tolerate ALTER TYPE ADD.
    // ========================================================================
    static coroutine::Task<DynamicArray<U64>> read_mask(Reader r, U64 count) {
        U64               mask_words = (count + 63_u64) / 64_u64;
        DynamicArray<U64> mask;
        resize(mask, mask_words);
        for (U64 i = 0; i < mask_words; i++) {
            co_await r(reinterpret_cast<U8*>(&mask[i]), sizeof(U64));
        }
        co_return mask;
    }

    static bool mask_bit(const DynamicArray<U64>& mask, U64 i) {
        return mask.length > 0 && ((mask[i / 64] >> (i % 64)) & 1_u64) != 0_u64;
    }

    static coroutine::Task<DynamicArray<NestedColumnValue>> read_masked_payloads(Reader r, const type::Type* field_types, U64 count) {
        DynamicArray<U64>               mask = co_await read_mask(r, count);
        DynamicArray<NestedColumnValue> arr;
        for (U64 i = 0; i < count; i++) {
            NestedColumnValue nv;
            if (mask_bit(mask, i)) {
                nv.value = co_await read_column_value(r, field_types[i]);
            } else {
                nv.value = {Null{}};
            }
            push_back(arr, move(nv));
        }
        co_return arr;
    }

    static coroutine::Task<void> skip_masked_payloads(Reader r, const type::Type* field_types, U64 count) {
        DynamicArray<U64> mask = co_await read_mask(r, count);
        for (U64 i = 0; i < count; i++) {
            if (mask_bit(mask, i)) {
                co_await skip_column_value(r, field_types[i]);
            }
        }
    }

    static void write_empty_mask(Writer w, U64 count) {
        U64 mask_words = (count + 63_u64) / 64_u64;
        U64 zero       = 0_u64;
        for (U64 i = 0; i < mask_words; i++) {
            w(reinterpret_cast<const U8*>(&zero), sizeof(zero));
        }
    }

    // Writes mask + payloads. The bitmask reflects, for i in [0, count), whether
    // `values[i]` (capped at values.length) is non-null. Out-of-range or null indices
    // contribute no payload bytes.
    static void write_masked_payloads(Writer w, const DynamicArray<NestedColumnValue>& values, const type::Type* field_types, U64 count) {
        U64               mask_words = (count + 63_u64) / 64_u64;
        DynamicArray<U64> mask;
        resize(mask, mask_words);
        for (U64 i = 0; i < values.length && i < count; i++) {
            if (!type_matches_tag<Null>(values[i].value)) {
                mask[i / 64] |= 1_u64 << (i % 64);
            }
        }
        for (U64 i = 0; i < mask_words; i++) {
            w(reinterpret_cast<const U8*>(&mask[i]), sizeof(U64));
        }
        for (U64 i = 0; i < values.length && i < count; i++) {
            if ((mask[i / 64] >> (i % 64) & 1_u64) == 0_u64) {
                continue;
            }
            write_column_value(w, values[i].value, field_types[i]);
        }
    }

    // UDT-specific wrappers that handle the leading written-count and the trailing
    // null-pad against the current type's arity (so ALTER TYPE ADD is tolerated on read).
    static coroutine::Task<DynamicArray<NestedColumnValue>> read_udt_fields(Reader r, const type::Type* field_types, U64 current_field_count) {
        U64 written = 0;
        co_await r(reinterpret_cast<U8*>(&written), sizeof(written));
        assert_true(written <= current_field_count, "UDT wire stream describes more fields than the current type's arity");
        DynamicArray<NestedColumnValue> arr = co_await read_masked_payloads(r, field_types, written);
        for (U64 i = written; i < current_field_count; i++) {
            NestedColumnValue nv;
            nv.value = {Null{}};
            push_back(arr, move(nv));
        }
        co_return arr;
    }

    static coroutine::Task<void> skip_udt_fields(Reader r, const type::Type* field_types, U64 current_field_count) {
        U64 written = 0;
        co_await r(reinterpret_cast<U8*>(&written), sizeof(written));
        assert_true(written <= current_field_count, "UDT skip: wire stream describes more fields than the current type's arity");
        co_await skip_masked_payloads(r, field_types, written);
    }

    coroutine::Task<ColumnValue> read_column_value(Reader r, const type::Type& cdtype) {
        if (type_matches_tag<type::Basic>(cdtype.value)) {
            co_return co_await read_column_value(r, get<type::Basic>(cdtype.value));
        }

        if (type_matches_tag<type::List>(cdtype.value)) {
            auto& v = get<type::List>(cdtype.value);
            U64   length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            DynamicArray<NestedColumnValue> arr{};
            for (U64 i = 0; i < length; i++) {
                NestedColumnValue nv;
                nv.value = co_await read_column_value(r, v.element);
                push_back(arr, move(nv));
            }
            co_return {move(arr)};
        }

        if (type_matches_tag<type::Set>(cdtype.value)) {
            auto& v = get<type::Set>(cdtype.value);
            U64   length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            DynamicSet<NestedColumnValue> s{};
            for (U64 i = 0; i < length; i++) {
                NestedColumnValue nv;
                nv.value = co_await read_column_value(r, v.key);
                insert(s, move(nv));
            }
            co_return {move(s)};
        }

        if (type_matches_tag<type::Map>(cdtype.value)) {
            auto& v = get<type::Map>(cdtype.value);
            U64   length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            DynamicMap<NestedColumnValue, NestedColumnValue> m{};
            for (U64 i = 0; i < length; i++) {
                NestedColumnValue k, val;
                k.value   = co_await read_column_value(r, v.key);
                val.value = co_await read_column_value(r, v.value);
                insert(m, move(k), move(val));
            }
            co_return {move(m)};
        }

        if (type_matches_tag<type::Vector>(cdtype.value)) {
            auto&                           v     = get<type::Vector>(cdtype.value);
            U64                             count = v.count;
            DynamicArray<NestedColumnValue> arr{};
            for (U64 i = 0; i < count; i++) {
                NestedColumnValue nv;
                nv.value = co_await read_column_value(r, v.element);
                push_back(arr, move(nv));
            }
            co_return {move(arr)};
        }

        if (type_matches_tag<type::Tuple>(cdtype.value)) {
            const auto&                     t   = get<type::Tuple>(cdtype.value);
            DynamicArray<NestedColumnValue> arr = co_await read_masked_payloads(r, t.elements.ptr, t.elements.length);
            co_return {move(arr)};
        }

        // UDT
        {
            type::UDT* u = get<type::UDT*>(cdtype.value);
            assert_true(u != nullptr, "UDT* column type is null");
            DynamicArray<NestedColumnValue> arr = co_await read_udt_fields(r, u->field_types.ptr, u->field_types.length);
            co_return {move(arr)};
        }
    }

    coroutine::Task<void> read_column_mask(Reader r, ColumnActiveSetter mark_active) {
        U64 column_count;
        co_await r(reinterpret_cast<U8*>(&column_count), sizeof(column_count));
        static_assert(sizeof(column_count) == COLUMN_COUNT_BYTE_COUNT);

        for (U64 mask_idx = 0; mask_idx < ceil_div(column_count, MASK_BIT_COUNT); mask_idx++) {
            U64 mask;
            co_await r(reinterpret_cast<U8*>(&mask), sizeof(mask));

            for (U64 bit_idx = 0; bit_idx < min(MASK_BIT_COUNT, column_count - mask_idx * MASK_BIT_COUNT); bit_idx++) {
                if (mask & (1_u64 << bit_idx)) {
                    mark_active(mask_idx * MASK_BIT_COUNT + bit_idx);
                }
            }
        }
    }

    // ========================================================================
    // skip
    // ========================================================================
    // skip_column_value(Reader, type::Basic) is generated in generated/io_codec_wire.cpp.

    coroutine::Task<void> skip_column_value(Reader r, const type::Type& col_type) {
        if (type_matches_tag<type::Basic>(col_type.value)) {
            co_await skip_column_value(r, get<type::Basic>(col_type.value));
            co_return;
        }
        if (type_matches_tag<type::List>(col_type.value)) {
            const auto& lv    = get<type::List>(col_type.value);
            U64         count = 0;
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            for (U64 i = 0; i < count; i++) {
                co_await skip_column_value(r, lv.element);
            }
            co_return;
        }
        if (type_matches_tag<type::Set>(col_type.value)) {
            const auto& sv    = get<type::Set>(col_type.value);
            U64         count = 0;
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            for (U64 i = 0; i < count; i++) {
                co_await skip_column_value(r, sv.key);
            }
            co_return;
        }
        if (type_matches_tag<type::Map>(col_type.value)) {
            const auto& mv    = get<type::Map>(col_type.value);
            U64         count = 0;
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            for (U64 i = 0; i < count; i++) {
                co_await skip_column_value(r, mv.key);
                co_await skip_column_value(r, mv.value);
            }
            co_return;
        }
        if (type_matches_tag<type::Vector>(col_type.value)) {
            const auto& vv = get<type::Vector>(col_type.value);
            for (U64 i = 0; i < vv.count; i++) {
                co_await skip_column_value(r, vv.element);
            }
            co_return;
        }
        if (type_matches_tag<type::Tuple>(col_type.value)) {
            const auto& t = get<type::Tuple>(col_type.value);
            co_await skip_masked_payloads(r, t.elements.ptr, t.elements.length);
            co_return;
        }
        if (type_matches_tag<type::UDT*>(col_type.value)) {
            type::UDT* u = get<type::UDT*>(col_type.value);
            assert_true(u != nullptr, "UDT* column type is null");
            co_await skip_udt_fields(r, u->field_types.ptr, u->field_types.length);
            co_return;
        }
    }

    // ========================================================================
    // write
    // ========================================================================
    // write_default_column_value(Writer, type::Basic) is generated in generated/io_codec_wire.cpp.

    void write_default_column_value(Writer w, const type::Type& cdtype) {
        visit(cdtype.value, [w](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                write_default_column_value(w, v);
            } else if constexpr (SameAs<T, type::Tuple>) {
                write_empty_mask(w, v.elements.length);
            } else if constexpr (SameAs<T, type::UDT*>) {
                assert_true(v != nullptr, "UDT* column type is null in default write");
                U64 field_count = v->field_types.length;
                w(reinterpret_cast<const U8*>(&field_count), sizeof(field_count));
                write_empty_mask(w, field_count);
            } else {
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }
        });
    }

    bool can_write_column_value(const ColumnValue& value, const type::Type& cdtype) {
        return visit(value, [&cdtype](const auto& v) -> bool {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return type_matches_tag<type::Basic>(cdtype.value) && can_write_typed_basic_as_column_value<T>(get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                return type_matches_tag<type::List>(cdtype.value) || type_matches_tag<type::Vector>(cdtype.value) || type_matches_tag<type::Tuple>(cdtype.value) || type_matches_tag<type::UDT*>(cdtype.value);
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                return type_matches_tag<type::Set>(cdtype.value);
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                return type_matches_tag<type::Map>(cdtype.value);
            } else if constexpr (SameAs<T, Null>) {
                return true;
            } else {
                static_assert(!SameAs<T, T>, "missing type case");
            }
        });
    }

    void write_column_value(Writer w, const ColumnValue& value, const type::Type& cdtype) {
        assert_true(can_write_column_value(value, cdtype), "invalid type to write for cdtype " + to_str(cdtype));
        visit(value, [&cdtype, w](auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                assert_true(type_matches_tag<type::Basic>(cdtype.value), "static value type requires ctype basic, this should never happen");
                write_typed_basic(w, v, get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                bool is_vec   = type_matches_tag<type::Vector>(cdtype.value);
                bool is_list  = type_matches_tag<type::List>(cdtype.value);
                bool is_tuple = type_matches_tag<type::Tuple>(cdtype.value);
                bool is_udt   = type_matches_tag<type::UDT*>(cdtype.value);
                assert_true(is_list || is_vec || is_tuple || is_udt, "array requires ctype list/vector/tuple/udt");
                if (is_tuple || is_udt) {
                    const type::Type* fields    = nullptr;
                    U64               field_cnt = 0;
                    if (is_tuple) {
                        const auto& t = get<type::Tuple>(cdtype.value);
                        fields        = t.elements.ptr;
                        field_cnt     = t.elements.length;
                    } else {
                        type::UDT* u = get<type::UDT*>(cdtype.value);
                        assert_true(u != nullptr, "UDT* column type is null");
                        fields    = u->field_types.ptr;
                        field_cnt = u->field_types.length;
                    }
                    if (is_udt) {
                        U64 fc = field_cnt;
                        w(reinterpret_cast<const U8*>(&fc), sizeof(fc));
                    }
                    write_masked_payloads(w, v, fields, field_cnt);
                } else {
                    if (!is_vec) {
                        U64 len = v.length;
                        w(reinterpret_cast<const U8*>(&len), sizeof(len));
                    }
                    const auto& elem = is_vec ? get<type::Vector>(cdtype.value).element : get<type::List>(cdtype.value).element;
                    for (const auto& el : v) {
                        write_column_value(w, el.value, elem);
                    }
                }
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                assert_true(type_matches_tag<type::Set>(cdtype.value), "set requires ctype set");
                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                const auto& key_type = get<type::Set>(cdtype.value).key;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    write_column_value(w, (*it).value, key_type);
                }
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                assert_true(type_matches_tag<type::Map>(cdtype.value), "map requires ctype map");
                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                const auto& m = get<type::Map>(cdtype.value);
                for (auto it = v.begin(); it != v.end(); ++it) {
                    write_column_value(w, (*it).first.value, m.key);
                    write_column_value(w, (*it).second.value, m.value);
                }
            } else if constexpr (SameAs<T, Null>) {
                // null column values are tracked via column mask; nothing to write
            } else {
                static_assert(!SameAs<T, T>, "unhandled read value type");
            }
        });
    }

    void write_column_mask(Writer w, ColumnActiveChecker is_active, U64 column_count) {
        w(reinterpret_cast<const U8*>(&column_count), sizeof(column_count));
        static_assert(sizeof(column_count) == COLUMN_COUNT_BYTE_COUNT);
        write_mask_bits(w, is_active, column_count);
    }

}
