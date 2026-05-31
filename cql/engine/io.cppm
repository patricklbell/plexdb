module;
#include <coroutine>

export module cql.engine.io;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;

export namespace cql::io {
    template<typename F>
    concept Read = requires(F f, U8* src, U64 size) {
        { f(src, size) } -> SameAs<coroutine::Task<void>>;
    };

    template<typename F>
    concept Write = requires(F f, const U8* src, U64 size) {
        f(src, size);
    };

    template<typename F>
    concept MarkColumnActive = requires(F f, U64 col_idx) {
        f(col_idx);
    };

    template<typename F>
    concept IsColumnActive = requires(F f, U64 col_idx) {
        f(col_idx);
    };

    constexpr U64 COLUMN_COUNT_BYTE_COUNT = sizeof(U64);
    constexpr U64 MASK_BYTE_COUNT = sizeof(U64);
    constexpr U64 MASK_BIT_COUNT = MASK_BYTE_COUNT*8_u64;

    // ========================================================================
    // read
    // ========================================================================
    coroutine::Task<ColumnValue> read_column_value(const Read auto& r, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::text:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));

                AutoString8 value{length};
                co_await r(reinterpret_cast<U8*>(value.c_str), length);

                co_return {move(value)};
            }break;
            case type::Basic::smallint:{
                S16 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::int_:{
                S32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::counter:
            case type::Basic::timestamp:
            case type::Basic::bigint:{
                S64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::boolean:{
                U8 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::float_:{
                F32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::double_:{
                F64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::uuid:{
                UUID value;
                co_await r(&value.value[0], value.length);

                co_return {move(value)};
            }break;
            case type::Basic::blob:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));

                Blob value;
                resize(value.value, length);
                assert_true(value.value.length == length, "resize successful");
                co_await r(value.value.ptr, length);

                co_return {move(value)};
            }
            case type::Basic::ascii:
            case type::Basic::varchar:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));

                AutoString8 value{length};
                co_await r(reinterpret_cast<U8*>(value.c_str), length);

                co_return {move(value)};
            }break;
            case type::Basic::tinyint:{
                U8 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::date:{
                S32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::time:{
                S64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case type::Basic::timeuuid:{
                UUID value;
                co_await r(&value.value[0], value.length);

                co_return {move(value)};
            }break;
            case type::Basic::inet:{
                U8 is_v6;
                co_await r(reinterpret_cast<U8*>(&is_v6), sizeof(is_v6));
                Inet value;
                value.is_v6 = static_cast<bool>(is_v6);
                if (value.is_v6) {
                    co_await r(&value.v6[0], 16);
                } else {
                    co_await r(&value.v4[0], 4);
                }
                co_return {move(value)};
            }break;
            case type::Basic::varint:{
                U8 negative;
                co_await r(reinterpret_cast<U8*>(&negative), sizeof(negative));
                U64 mag_length;
                co_await r(reinterpret_cast<U8*>(&mag_length), sizeof(mag_length));
                VarInt value;
                value.negative = static_cast<bool>(negative);
                resize(value.magnitude, mag_length);
                co_await r(value.magnitude.ptr, mag_length);
                co_return {move(value)};
            }break;
            case type::Basic::decimal:{
                S32 scale;
                co_await r(reinterpret_cast<U8*>(&scale), sizeof(scale));
                U8 negative;
                co_await r(reinterpret_cast<U8*>(&negative), sizeof(negative));
                U64 mag_length;
                co_await r(reinterpret_cast<U8*>(&mag_length), sizeof(mag_length));
                Decimal value;
                value.scale = scale;
                value.unscaled.negative = static_cast<bool>(negative);
                resize(value.unscaled.magnitude, mag_length);
                co_await r(value.unscaled.magnitude.ptr, mag_length);
                co_return {move(value)};
            }break;
            case type::Basic::duration:{
                Duration value;
                co_await r(reinterpret_cast<U8*>(&value.months), sizeof(value.months));
                co_await r(reinterpret_cast<U8*>(&value.days), sizeof(value.days));
                co_await r(reinterpret_cast<U8*>(&value.nanoseconds), sizeof(value.nanoseconds));
                co_return {move(value)};
            }break;
            case type::Basic::hex:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
                Blob value;
                resize(value.value, length);
                assert_true(value.value.length == length, "resize successful");
                co_await r(value.value.ptr, length);
                co_return {move(value)};
            }break;
        }

        assert_true(false, "invalid basic type for read");
        co_return {};
    }

    coroutine::Task<ColumnValue> read_column_value(const Read auto& r, const type::Type& cdtype) {
        if (type_matches_tag<type::Basic>(cdtype.value)) {
            co_return co_await read_column_value(r, get<type::Basic>(cdtype.value));
        }

        if (type_matches_tag<type::List>(cdtype.value)) {
            auto& v = get<type::List>(cdtype.value);
            U64 length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            if (!type_matches_tag<type::Basic>(v.element.value)) {
                DynamicArray<NestedColumnValue> arr{};
                for (U64 i = 0; i < length; i++) {
                    NestedColumnValue nv;
                    nv.value = co_await read_column_value(r, v.element);
                    push_back(arr, move(nv));
                }
                co_return {move(arr)};
            }
            auto read_arr = [&]<typename El>() -> coroutine::Task<ColumnValue> {
                DynamicArray<El> arr{};
                for (U64 i = 0; i < length; i++)
                    push_back(arr, get<El>(co_await read_column_value(r, get<type::Basic>(v.element.value))));
                co_return {move(arr)};
            };
            switch (get<type::Basic>(v.element.value)) {
                case type::Basic::text:       co_return co_await read_arr.template operator()<AutoString8>();
                case type::Basic::smallint:   co_return co_await read_arr.template operator()<S16>();
                case type::Basic::int_:       co_return co_await read_arr.template operator()<S32>();
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time:       co_return co_await read_arr.template operator()<S64>();
                case type::Basic::boolean:    co_return co_await read_arr.template operator()<U8>();
                case type::Basic::float_:     co_return co_await read_arr.template operator()<F32>();
                case type::Basic::double_:    co_return co_await read_arr.template operator()<F64>();
                case type::Basic::uuid:
                case type::Basic::timeuuid:   co_return co_await read_arr.template operator()<UUID>();
                case type::Basic::tinyint:    co_return co_await read_arr.template operator()<U8>();
                case type::Basic::date:       co_return co_await read_arr.template operator()<S32>();
                case type::Basic::ascii:
                case type::Basic::varchar:    co_return co_await read_arr.template operator()<AutoString8>();
                case type::Basic::blob:       co_return co_await read_arr.template operator()<Blob>();
                case type::Basic::hex:        co_return co_await read_arr.template operator()<Blob>();
                case type::Basic::inet:       co_return co_await read_arr.template operator()<Inet>();
                case type::Basic::varint:     co_return co_await read_arr.template operator()<VarInt>();
                case type::Basic::decimal:    co_return co_await read_arr.template operator()<Decimal>();
                case type::Basic::duration:   co_return co_await read_arr.template operator()<Duration>();
            }
            assert_true(false, "invalid basic type for list read");
            co_return {};
        }

        if (type_matches_tag<type::Set>(cdtype.value)) {
            auto& v = get<type::Set>(cdtype.value);
            U64 length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            if (!type_matches_tag<type::Basic>(v.key.value)) {
                assert_not_implemented("reading nested-element set is not implemented");
                co_return {};
            }
            auto read_set = [&]<typename K>() -> coroutine::Task<ColumnValue> {
                DynamicSet<K> s{};
                for (U64 i = 0; i < length; i++)
                    insert(s, get<K>(co_await read_column_value(r, get<type::Basic>(v.key.value))));
                co_return {move(s)};
            };
            switch (get<type::Basic>(v.key.value)) {
                case type::Basic::text:       co_return co_await read_set.template operator()<AutoString8>();
                case type::Basic::smallint:   co_return co_await read_set.template operator()<S16>();
                case type::Basic::int_:       co_return co_await read_set.template operator()<S32>();
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time:       co_return co_await read_set.template operator()<S64>();
                case type::Basic::boolean:    co_return co_await read_set.template operator()<U8>();
                case type::Basic::float_:     co_return co_await read_set.template operator()<F32>();
                case type::Basic::double_:    co_return co_await read_set.template operator()<F64>();
                case type::Basic::uuid:
                case type::Basic::timeuuid:   co_return co_await read_set.template operator()<UUID>();
                case type::Basic::tinyint:    co_return co_await read_set.template operator()<U8>();
                case type::Basic::date:       co_return co_await read_set.template operator()<S32>();
                case type::Basic::ascii:
                case type::Basic::varchar:    co_return co_await read_set.template operator()<AutoString8>();
                case type::Basic::blob:       co_return co_await read_set.template operator()<Blob>();
                case type::Basic::hex:        co_return co_await read_set.template operator()<Blob>();
                case type::Basic::inet:       co_return co_await read_set.template operator()<Inet>();
                case type::Basic::varint:     co_return co_await read_set.template operator()<VarInt>();
                case type::Basic::decimal:    co_return co_await read_set.template operator()<Decimal>();
                case type::Basic::duration:   co_return co_await read_set.template operator()<Duration>();
            }
            assert_true(false, "invalid basic type for set read");
            co_return {};
        }

        if (type_matches_tag<type::Map>(cdtype.value)) {
            auto& v = get<type::Map>(cdtype.value);
            U64 length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            if (!type_matches_tag<type::Basic>(v.key.value) || !type_matches_tag<type::Basic>(v.value.value)) {
                assert_not_implemented("reading nested-element map is not implemented");
                co_return {};
            }
            auto read_map_kv = [&]<typename K, typename V>() -> coroutine::Task<ColumnValue> {
                DynamicMap<K, V> m{};
                for (U64 i = 0; i < length; i++) {
                    K k = get<K>(co_await read_column_value(r, get<type::Basic>(v.key.value)));
                    V val = get<V>(co_await read_column_value(r, get<type::Basic>(v.value.value)));
                    insert(m, move(k), move(val));
                }
                co_return {move(m)};
            };
            auto read_map_v = [&]<typename K>(type::Basic val_dtype) -> coroutine::Task<ColumnValue> {
                switch (val_dtype) {
                    case type::Basic::text:       co_return co_await read_map_kv.template operator()<K, AutoString8>();
                    case type::Basic::smallint:   co_return co_await read_map_kv.template operator()<K, S16>();
                    case type::Basic::int_:       co_return co_await read_map_kv.template operator()<K, S32>();
                    case type::Basic::bigint:
                    case type::Basic::timestamp:
                    case type::Basic::counter:
                    case type::Basic::time:       co_return co_await read_map_kv.template operator()<K, S64>();
                    case type::Basic::boolean:    co_return co_await read_map_kv.template operator()<K, U8>();
                    case type::Basic::float_:     co_return co_await read_map_kv.template operator()<K, F32>();
                    case type::Basic::double_:    co_return co_await read_map_kv.template operator()<K, F64>();
                    case type::Basic::uuid:
                    case type::Basic::timeuuid:   co_return co_await read_map_kv.template operator()<K, UUID>();
                    case type::Basic::tinyint:    co_return co_await read_map_kv.template operator()<K, U8>();
                    case type::Basic::date:       co_return co_await read_map_kv.template operator()<K, S32>();
                    case type::Basic::ascii:
                    case type::Basic::varchar:    co_return co_await read_map_kv.template operator()<K, AutoString8>();
                    case type::Basic::blob:       co_return co_await read_map_kv.template operator()<K, Blob>();
                    case type::Basic::hex:        co_return co_await read_map_kv.template operator()<K, Blob>();
                    case type::Basic::inet:       co_return co_await read_map_kv.template operator()<K, Inet>();
                    case type::Basic::varint:     co_return co_await read_map_kv.template operator()<K, VarInt>();
                    case type::Basic::decimal:    co_return co_await read_map_kv.template operator()<K, Decimal>();
                    case type::Basic::duration:   co_return co_await read_map_kv.template operator()<K, Duration>();
                }
                assert_true(false, "invalid basic type for map value read");
                co_return {};
            };
            type::Basic map_val_bt = get<type::Basic>(v.value.value);
            switch (get<type::Basic>(v.key.value)) {
                case type::Basic::text:       co_return co_await read_map_v.template operator()<AutoString8>(map_val_bt);
                case type::Basic::smallint:   co_return co_await read_map_v.template operator()<S16>(map_val_bt);
                case type::Basic::int_:       co_return co_await read_map_v.template operator()<S32>(map_val_bt);
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time:       co_return co_await read_map_v.template operator()<S64>(map_val_bt);
                case type::Basic::boolean:    co_return co_await read_map_v.template operator()<U8>(map_val_bt);
                case type::Basic::float_:     co_return co_await read_map_v.template operator()<F32>(map_val_bt);
                case type::Basic::double_:    co_return co_await read_map_v.template operator()<F64>(map_val_bt);
                case type::Basic::uuid:
                case type::Basic::timeuuid:   co_return co_await read_map_v.template operator()<UUID>(map_val_bt);
                case type::Basic::tinyint:    co_return co_await read_map_v.template operator()<U8>(map_val_bt);
                case type::Basic::date:       co_return co_await read_map_v.template operator()<S32>(map_val_bt);
                case type::Basic::ascii:
                case type::Basic::varchar:    co_return co_await read_map_v.template operator()<AutoString8>(map_val_bt);
                case type::Basic::blob:       co_return co_await read_map_v.template operator()<Blob>(map_val_bt);
                case type::Basic::hex:        co_return co_await read_map_v.template operator()<Blob>(map_val_bt);
                case type::Basic::inet:       co_return co_await read_map_v.template operator()<Inet>(map_val_bt);
                case type::Basic::varint:     co_return co_await read_map_v.template operator()<VarInt>(map_val_bt);
                case type::Basic::decimal:    co_return co_await read_map_v.template operator()<Decimal>(map_val_bt);
                case type::Basic::duration:   co_return co_await read_map_v.template operator()<Duration>(map_val_bt);
            }
            assert_true(false, "invalid basic type for map key read");
            co_return {};
        }

        // Vector
        {
            auto& v = get<type::Vector>(cdtype.value);
            U64 count = v.count;
            if (!type_matches_tag<type::Basic>(v.element.value)) {
                DynamicArray<NestedColumnValue> arr{};
                for (U64 i = 0; i < count; i++) {
                    NestedColumnValue nv;
                    nv.value = co_await read_column_value(r, v.element);
                    push_back(arr, move(nv));
                }
                co_return {move(arr)};
            }
            auto read_vec = [&]<typename El>() -> coroutine::Task<ColumnValue> {
                DynamicArray<El> arr{};
                for (U64 i = 0; i < count; i++)
                    push_back(arr, get<El>(co_await read_column_value(r, get<type::Basic>(v.element.value))));
                co_return {move(arr)};
            };
            switch (get<type::Basic>(v.element.value)) {
                case type::Basic::text:       co_return co_await read_vec.template operator()<AutoString8>();
                case type::Basic::smallint:   co_return co_await read_vec.template operator()<S16>();
                case type::Basic::int_:       co_return co_await read_vec.template operator()<S32>();
                case type::Basic::bigint:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::time:       co_return co_await read_vec.template operator()<S64>();
                case type::Basic::boolean:    co_return co_await read_vec.template operator()<U8>();
                case type::Basic::float_:     co_return co_await read_vec.template operator()<F32>();
                case type::Basic::double_:    co_return co_await read_vec.template operator()<F64>();
                case type::Basic::uuid:
                case type::Basic::timeuuid:   co_return co_await read_vec.template operator()<UUID>();
                case type::Basic::tinyint:    co_return co_await read_vec.template operator()<U8>();
                case type::Basic::date:       co_return co_await read_vec.template operator()<S32>();
                case type::Basic::ascii:
                case type::Basic::varchar:    co_return co_await read_vec.template operator()<AutoString8>();
                case type::Basic::blob:       co_return co_await read_vec.template operator()<Blob>();
                case type::Basic::hex:        co_return co_await read_vec.template operator()<Blob>();
                case type::Basic::inet:       co_return co_await read_vec.template operator()<Inet>();
                case type::Basic::varint:     co_return co_await read_vec.template operator()<VarInt>();
                case type::Basic::decimal:    co_return co_await read_vec.template operator()<Decimal>();
                case type::Basic::duration:   co_return co_await read_vec.template operator()<Duration>();
            }
            assert_true(false, "invalid basic type for vector read");
            co_return {};
        }
    }

    coroutine::Task<void> read_column_mask(const Read auto& r, const MarkColumnActive auto& mark_active) {
        U64 column_count;
        co_await r(reinterpret_cast<U8*>(&column_count), sizeof(column_count));
        static_assert(sizeof(column_count) == COLUMN_COUNT_BYTE_COUNT);

        for (U64 mask_idx = 0; mask_idx < ceil_div(column_count, MASK_BIT_COUNT); mask_idx++) {
            U64 mask;
            co_await r(reinterpret_cast<U8*>(&mask), sizeof(mask));

            for (U64 bit_idx = 0; bit_idx < min(MASK_BIT_COUNT, column_count - mask_idx*MASK_BIT_COUNT); bit_idx++) {
                if (mask & (1_u64 << bit_idx)) {
                    mark_active(mask_idx*MASK_BIT_COUNT + bit_idx);
                }
            }
        }
    }

    // ========================================================================
    // skip
    // ========================================================================
    coroutine::Task<void> skip_column_value(const Read auto& r, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::boolean:
            case type::Basic::tinyint:   co_await r(nullptr, 1);             co_return;
            case type::Basic::smallint:  co_await r(nullptr, sizeof(S16));   co_return;
            case type::Basic::int_:
            case type::Basic::date:
            case type::Basic::float_:    co_await r(nullptr, sizeof(S32));   co_return;
            case type::Basic::bigint:
            case type::Basic::counter:
            case type::Basic::timestamp:
            case type::Basic::time:
            case type::Basic::double_:   co_await r(nullptr, sizeof(S64));   co_return;
            case type::Basic::uuid:
            case type::Basic::timeuuid:  co_await r(nullptr, 16);            co_return;
            case type::Basic::duration:  co_await r(nullptr, 4+4+8);         co_return;
            case type::Basic::text:
            case type::Basic::ascii:
            case type::Basic::varchar:
            case type::Basic::blob:
            case type::Basic::hex: {
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(U64));
                co_await r(nullptr, length);
                co_return;
            }
            case type::Basic::varint: {
                co_await r(nullptr, 1);  // negative byte
                U64 mag_length;
                co_await r(reinterpret_cast<U8*>(&mag_length), sizeof(U64));
                co_await r(nullptr, mag_length);
                co_return;
            }
            case type::Basic::decimal: {
                co_await r(nullptr, sizeof(S32) + 1);  // scale + negative byte
                U64 mag_length;
                co_await r(reinterpret_cast<U8*>(&mag_length), sizeof(U64));
                co_await r(nullptr, mag_length);
                co_return;
            }
            case type::Basic::inet: {
                U8 is_v6;
                co_await r(&is_v6, 1);
                co_await r(nullptr, is_v6 ? 16_u64 : 4_u64);
                co_return;
            }
        }
        assert_true(false, "invalid basic type in skip_column_value");
    }

    coroutine::Task<void> skip_column_value(const Read auto& r, const type::Type& col_type) {
        if (type_matches_tag<type::Basic>(col_type.value)) {
            co_await skip_column_value(r, get<type::Basic>(col_type.value));
            co_return;
        }

        U64 count = 0;
        type::Basic elem_bt{};
        bool elem_is_type = false;
        const type::Type* elem_type_ptr = nullptr;

        if (type_matches_tag<type::List>(col_type.value)) {
            auto& lv = get<type::List>(col_type.value);
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            elem_is_type = !type_matches_tag<type::Basic>(lv.element.value);
            if (elem_is_type) elem_type_ptr = &lv.element;
            else              elem_bt = get<type::Basic>(lv.element.value);
        } else if (type_matches_tag<type::Set>(col_type.value)) {
            auto& sv = get<type::Set>(col_type.value);
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            elem_is_type = !type_matches_tag<type::Basic>(sv.key.value);
            if (elem_is_type) elem_type_ptr = &sv.key;
            else              elem_bt = get<type::Basic>(sv.key.value);
        } else if (type_matches_tag<type::Map>(col_type.value)) {
            auto& mv = get<type::Map>(col_type.value);
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            bool key_is_type = !type_matches_tag<type::Basic>(mv.key.value);
            bool val_is_type = !type_matches_tag<type::Basic>(mv.value.value);
            type::Basic kbt = key_is_type ? type::Basic{} : get<type::Basic>(mv.key.value);
            type::Basic vbt = val_is_type ? type::Basic{} : get<type::Basic>(mv.value.value);
            for (U64 i = 0; i < count; i++) {
                if (key_is_type) co_await skip_column_value(r, mv.key);
                else             co_await skip_column_value(r, kbt);
                if (val_is_type) co_await skip_column_value(r, mv.value);
                else             co_await skip_column_value(r, vbt);
            }
            co_return;
        } else if (type_matches_tag<type::Vector>(col_type.value)) {
            auto& vv = get<type::Vector>(col_type.value);
            count = vv.count;
            elem_is_type = !type_matches_tag<type::Basic>(vv.element.value);
            if (elem_is_type) elem_type_ptr = &vv.element;
            else              elem_bt = get<type::Basic>(vv.element.value);
        }

        for (U64 i = 0; i < count; i++) {
            if (elem_is_type) co_await skip_column_value(r, *elem_type_ptr);
            else              co_await skip_column_value(r, elem_bt);
        }
    }

    // ========================================================================
    // write
    // ========================================================================
    template<typename T>
    bool can_write_typed_basic_as_column_value(type::Basic dtype) {
        if constexpr (Either<RemoveCV<T>, AutoString8>)   { return dtype == type::Basic::text || dtype == type::Basic::ascii || dtype == type::Basic::varchar; }
        else if constexpr (Either<RemoveCV<T>, S16>)      { return dtype == type::Basic::smallint; }
        else if constexpr (Either<RemoveCV<T>, S32>)      { return dtype == type::Basic::int_ || dtype == type::Basic::date; }
        else if constexpr (Either<RemoveCV<T>, S64>)      { return dtype == type::Basic::bigint || dtype == type::Basic::timestamp || dtype == type::Basic::counter || dtype == type::Basic::time || dtype == type::Basic::int_ || dtype == type::Basic::smallint || dtype == type::Basic::tinyint || dtype == type::Basic::date; }
        else if constexpr (Either<RemoveCV<T>, U8>)       { return dtype == type::Basic::boolean || dtype == type::Basic::tinyint; }
        else if constexpr (Either<RemoveCV<T>, F32>)      { return dtype == type::Basic::float_; }
        else if constexpr (Either<RemoveCV<T>, F64>)      { return dtype == type::Basic::double_; }
        else if constexpr (Either<RemoveCV<T>, UUID>)     { return dtype == type::Basic::uuid || dtype == type::Basic::timeuuid; }
        else if constexpr (Either<RemoveCV<T>, Null>)     { return true; }
        else if constexpr (Either<RemoveCV<T>, bool>)     { return dtype == type::Basic::boolean; }
        else if constexpr (Either<RemoveCV<T>, Hex>)      { return dtype == type::Basic::hex; }
        else if constexpr (Either<RemoveCV<T>, Blob>)     { return dtype == type::Basic::blob || dtype == type::Basic::hex; }
        else if constexpr (Either<RemoveCV<T>, Inet>)     { return dtype == type::Basic::inet; }
        else if constexpr (Either<RemoveCV<T>, VarInt>)   { return dtype == type::Basic::varint; }
        else if constexpr (Either<RemoveCV<T>, Decimal>)  { return dtype == type::Basic::decimal; }
        else if constexpr (Either<RemoveCV<T>, Duration>) { return dtype == type::Basic::duration; }
        else                                              { static_assert(!SameAs<T,T>, "missing type case"); return false; }
    }

    bool can_cast_write_constant_as_column_value(const Constant& constant, type::Basic dtype) {
        return visit(constant.value, [&](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            return can_write_typed_basic_as_column_value<T>(dtype);
        });
    }

    bool can_write_column_value(const ColumnValue& value, const type::Type& cdtype);

    bool can_cast_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype) {
        return visit(evaluated.value, [&cdtype](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                return type_matches_tag<type::Basic>(cdtype.value) &&
                       can_cast_write_constant_as_column_value(cv, get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, MapLiteral>) {
                return type_matches_tag<type::Map>(cdtype.value);
            } else if constexpr (SameAs<T, SetLiteral>) {
                return type_matches_tag<type::Set>(cdtype.value);
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                return type_matches_tag<type::List>(cdtype.value) || type_matches_tag<type::Vector>(cdtype.value);
            } else if constexpr (SameAs<T, UdtLiteral>) {
                assert_not_implemented("writing UDT literal as column value is not implemented");
                return false;
            } else if constexpr (SameAs<T, TupleLiteral>) {
                assert_not_implemented("writing tuple literal as column value is not implemented");
                return false;
            } else if constexpr (SameAs<T, ColumnValue>) {
                return can_write_column_value(cv, cdtype);
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
                return false;
            }
        });
    }


    // @todo write null
    // @todo avoid copy for zero init
    void write_default_column_value(const Write auto& w, type::Basic dtype) {
        switch (dtype) {
            case type::Basic::text:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
            case type::Basic::smallint:{
                S16 smallint = 0_s16;
                w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
            }break;
            case type::Basic::int_:{
                S32 int_ = 0_s32;
                w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
            }break;
            case type::Basic::counter:
            case type::Basic::bigint:{
                S64 bigint = 0_s64;
                w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
            }break;
            case type::Basic::boolean:{
                U8 boolean = false;
                w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
            }break;
            case type::Basic::float_:{
                F32 float_ = 0_f32;
                w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
            }break;
            case type::Basic::double_:{
                F64 double_ = 0_f64;
                w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
            }break;
            case type::Basic::uuid:{
                UUID uuid{};
                w(&uuid.value[0], uuid.value.length);
            }break;
            case type::Basic::blob:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
            case type::Basic::timestamp:
            case type::Basic::time:
            case type::Basic::ascii:
            case type::Basic::varchar:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
            case type::Basic::tinyint:{
                U8 tinyint = U8(0);
                w(reinterpret_cast<const U8*>(&tinyint), sizeof(tinyint));
            }break;
            case type::Basic::date:{
                S32 date = 0_s32;
                w(reinterpret_cast<const U8*>(&date), sizeof(date));
            }break;
            case type::Basic::timeuuid:{
                UUID uuid{};
                w(&uuid.value[0], uuid.length);
            }break;
            case type::Basic::inet:{
                U8 is_v6 = 0;
                w(reinterpret_cast<const U8*>(&is_v6), sizeof(is_v6));
                U8 zeros[4] = {0, 0, 0, 0};
                w(zeros, 4);
            }break;
            case type::Basic::varint:{
                U8 negative = 0;
                w(reinterpret_cast<const U8*>(&negative), sizeof(negative));
                U64 mag_length = 0;
                w(reinterpret_cast<const U8*>(&mag_length), sizeof(mag_length));
            }break;
            case type::Basic::decimal:{
                S32 scale = 0;
                w(reinterpret_cast<const U8*>(&scale), sizeof(scale));
                U8 negative = 0;
                w(reinterpret_cast<const U8*>(&negative), sizeof(negative));
                U64 mag_length = 0;
                w(reinterpret_cast<const U8*>(&mag_length), sizeof(mag_length));
            }break;
            case type::Basic::duration:{
                S32 zero32 = 0;
                S64 zero64 = 0;
                w(reinterpret_cast<const U8*>(&zero32), sizeof(zero32));
                w(reinterpret_cast<const U8*>(&zero32), sizeof(zero32));
                w(reinterpret_cast<const U8*>(&zero64), sizeof(zero64));
            }break;
            case type::Basic::hex:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
        }
    }

    void write_default_column_value(const Write auto& w, const type::Type& cdtype) {
        visit(cdtype.value, [&w](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, type::Basic>) {
                write_default_column_value(w, v);
            } else {
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }
        });
    }

    template<typename T>
    void write_typed_basic_as_column_value(const Write auto& w, const T& src, type::Basic dtype) {
        if constexpr (SameAs<T, AutoString8>) {
            switch (dtype) {
                case type::Basic::text:
                case type::Basic::ascii:
                case type::Basic::varchar:{
                    w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                    w(reinterpret_cast<const U8*>(src.c_str), src.length);
                }break;
                case type::Basic::inet:
                case type::Basic::varint:
                case type::Basic::decimal:
                case type::Basic::duration:
                    case type::Basic::hex:{
                    w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                    w(reinterpret_cast<const U8*>(src.c_str), src.length);
                }break;
                default:{ assert_not_implemented("writing string-like value to this dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, S64>) {
            switch (dtype) {
                case type::Basic::tinyint:{
                    U8 tinyint = static_cast<U8>(static_cast<S8>(src));
                    w(reinterpret_cast<const U8*>(&tinyint), sizeof(tinyint));
                }break;
                case type::Basic::smallint:{
                    S16 smallint = static_cast<S16>(src);
                    w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                }break;
                case type::Basic::int_:{
                    S32 int_ = static_cast<S32>(src);
                    w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                }break;
                case type::Basic::date:{
                    S32 date = static_cast<S32>(src);
                    w(reinterpret_cast<const U8*>(&date), sizeof(date));
                }break;
                case type::Basic::time:
                case type::Basic::timestamp:
                case type::Basic::counter:
                case type::Basic::bigint:{
                    S64 bigint = static_cast<S64>(src);
                    w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                }break;
                default:{ assert_not_implemented("writing integer value to this dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, bool>) {
            switch (dtype) {
                case type::Basic::boolean:{
                    U8 boolean = static_cast<U8>(src);
                    w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                }break;
                default:{ assert_not_implemented("writing boolean value to non-boolean dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, F64>) {
            switch (dtype) {
                case type::Basic::float_:{
                    F32 float_ = static_cast<F32>(src);
                    w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                }break;
                case type::Basic::double_:{
                    F64 double_ = static_cast<F64>(src);
                    w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                }break;
                default:{ assert_not_implemented("writing float value to non-float dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, UUID>) {
            assert_true(dtype == type::Basic::uuid || dtype == type::Basic::timeuuid, "UUID value written to non-uuid column");
            w(&src.value[0], src.length);
        } else if constexpr (SameAs<T, Hex>) {
            assert_true(dtype == type::Basic::hex, "hex value written to non-hex column");
            w(reinterpret_cast<const U8*>(&src.value.length), sizeof(src.value.length));
            w(&src.value[0], src.value.length);
        } else if constexpr (SameAs<T, Blob>) {
            assert_true(dtype == type::Basic::blob || dtype == type::Basic::inet || dtype == type::Basic::varint ||
                        dtype == type::Basic::decimal || dtype == type::Basic::duration ||
                        dtype == type::Basic::hex, "blob value written to incompatible column dtype");
            w(reinterpret_cast<const U8*>(&src.value.length), sizeof(src.value.length));
            w(&src.value[0], src.value.length);
        } else if constexpr (SameAs<T, Null>) {
            assert_not_implemented("writing null column values is not implemented");
        } else {
            static_assert(!SameAs<T,T>, "missing underlying type case");
        }
    }

    void cast_write_constant_as_column_value(const Write auto& w, const Constant& constant, type::Basic dtype) {
        visit(constant.value, [&](const auto& cv) {
            using T = Decay<decltype(cv)>;
            write_typed_basic_as_column_value<T>(w, cv, dtype);
        });
    }

    bool can_cast_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype);

    void write_column_value(const Write auto& w, const ColumnValue& value, const type::Type& cdtype);

    void cast_write_evaluated_as_column_value(const Write auto& w, const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx) {
        assert_true(can_cast_write_evaluated_as_column_value(evaluated, cdtype), "invalid evaluated value for write");

        visit(evaluated.value, [&](const auto& cv) {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                assert_true(type_matches_tag<type::Basic>(cdtype.value), "bad can write check!");
                cast_write_constant_as_column_value(w, cv, get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, MapLiteral>) {
                assert_true(type_matches_tag<type::Map>(cdtype.value), "map literal written to non-map column");
                auto& m = get<type::Map>(cdtype.value);
                type::Type key_type = m.key;
                type::Type val_type = m.value;
                U64 len = cv.key_values.length;
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (U64 i = 0; i < len; i++) {
                    Evaluated k = evaluate(cv.key_values[i].first, ctx);
                    Evaluated v = evaluate(cv.key_values[i].second, ctx);
                    cast_write_evaluated_as_column_value(w, k, key_type, ctx);
                    cast_write_evaluated_as_column_value(w, v, val_type, ctx);
                }
            } else if constexpr (SameAs<T, SetLiteral>) {
                assert_true(type_matches_tag<type::Set>(cdtype.value), "set literal written to non-set column");
                type::Type key_type = get<type::Set>(cdtype.value).key;
                U64 len = cv.keys.length;
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (U64 i = 0; i < len; i++) {
                    Evaluated el = evaluate(cv.keys[i], ctx);
                    cast_write_evaluated_as_column_value(w, el, key_type, ctx);
                }
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                bool is_vec = type_matches_tag<type::Vector>(cdtype.value);
                bool is_list = type_matches_tag<type::List>(cdtype.value);
                assert_true(is_list || is_vec, "list/vector literal written to incompatible column");
                if (is_list) {
                    U64 len = cv.elements.length;
                    w(reinterpret_cast<const U8*>(&len), sizeof(len));
                }
                type::Type el_type = get<type::List>(cdtype.value).element;
                if (is_vec) {
                    el_type = get<type::Vector>(cdtype.value).element;
                }
                for (U64 i = 0; i < cv.elements.length; i++) {
                    Evaluated el = evaluate(cv.elements[i], ctx);
                    cast_write_evaluated_as_column_value(w, el, el_type, ctx);
                }
            } else if constexpr (SameAs<T, UdtLiteral>) {
                assert_not_implemented("writing UDT literal as column value is not implemented");
            } else if constexpr (SameAs<T, TupleLiteral>) {
                assert_not_implemented("writing tuple literal as column value is not implemented");
            } else if constexpr (SameAs<T, ColumnValue>) {
                write_column_value(w, cv, cdtype);
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
            }
        });
    }

    void cast_write_evaluated_as_column_value(const Write auto& w, const Evaluated& evaluated, const type::Type& cdtype) {
        cast_write_evaluated_as_column_value(w, evaluated, cdtype, EvalContext{});
    }

    bool can_write_column_value(const ColumnValue& value, const type::Type& cdtype) {
        return visit(value, [&cdtype](const auto& v) -> bool {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return type_matches_tag<type::Basic>(cdtype.value) &&
                       can_write_typed_basic_as_column_value<T>(get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                bool is_list = type_matches_tag<type::List>(cdtype.value);
                bool is_vec  = type_matches_tag<type::Vector>(cdtype.value);
                if (!is_list && !is_vec) return false;
                auto& elem = is_vec ? get<type::Vector>(cdtype.value).element : get<type::List>(cdtype.value).element;
                return !type_matches_tag<type::Basic>(elem.value);
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                return type_matches_tag<type::Set>(cdtype.value) &&
                       !type_matches_tag<type::Basic>(get<type::Set>(cdtype.value).key.value);
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                if (!type_matches_tag<type::Map>(cdtype.value)) return false;
                auto& m = get<type::Map>(cdtype.value);
                return !type_matches_tag<type::Basic>(m.key.value) && !type_matches_tag<type::Basic>(m.value.value);
            } else if constexpr (IsDynamicMap<T>) {
                if (!type_matches_tag<type::Map>(cdtype.value)) return false;
                auto& m = get<type::Map>(cdtype.value);
                return type_matches_tag<type::Basic>(m.key.value) && type_matches_tag<type::Basic>(m.value.value) &&
                       can_write_typed_basic_as_column_value<typename T::Key>(get<type::Basic>(m.key.value)) &&
                       can_write_typed_basic_as_column_value<typename T::Value>(get<type::Basic>(m.value.value));
            } else if constexpr (IsDynamicSet<T>) {
                if (!type_matches_tag<type::Set>(cdtype.value)) return false;
                auto& s = get<type::Set>(cdtype.value);
                return type_matches_tag<type::Basic>(s.key.value) &&
                       can_write_typed_basic_as_column_value<typename T::Key>(get<type::Basic>(s.key.value));
            } else if constexpr (IsDynamicArray<T>) {
                bool is_list = type_matches_tag<type::List>(cdtype.value);
                bool is_vec  = type_matches_tag<type::Vector>(cdtype.value);
                if (!is_list && !is_vec) return false;
                auto& elem = is_vec ? get<type::Vector>(cdtype.value).element : get<type::List>(cdtype.value).element;
                return type_matches_tag<type::Basic>(elem.value) &&
                       can_write_typed_basic_as_column_value<typename T::Element>(get<type::Basic>(elem.value));
            } else if constexpr (SameAs<T, Null>) {
                return true;
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
            }
        });
    }

    template<typename T>
    void write_typed_basic(const Write auto& w, const T& src, [[maybe_unused]] type::Basic dtype) {
        if constexpr (SameAs<T, AutoString8>) {
            String8 s{src};
            w(reinterpret_cast<const U8*>(&s.length), sizeof(s.length));
            w(reinterpret_cast<const U8*>(s.data), s.length);
        } else if constexpr (Either<T,Array<U8, 16>, S64, S32, S16, U8, F64, F32>) {
            w(reinterpret_cast<const U8*>(&src), sizeof(src));
        } else if constexpr (SameAs<T, UUID>) {
            w(&src.value[0], src.length);
        } else if constexpr (SameAs<T, Blob>) {
            w(reinterpret_cast<const U8*>(&src.value.length), sizeof(src.value.length));
            w(src.value.ptr, src.value.length);
        } else if constexpr (SameAs<T, Inet>) {
            U8 is_v6 = src.is_v6 ? 1 : 0;
            w(reinterpret_cast<const U8*>(&is_v6), sizeof(is_v6));
            if (src.is_v6) {
                w(&src.v6[0], 16);
            } else {
                w(&src.v4[0], 4);
            }
        } else if constexpr (SameAs<T, VarInt>) {
            U8 negative = src.negative ? 1 : 0;
            w(reinterpret_cast<const U8*>(&negative), sizeof(negative));
            w(reinterpret_cast<const U8*>(&src.magnitude.length), sizeof(src.magnitude.length));
            w(src.magnitude.ptr, src.magnitude.length);
        } else if constexpr (SameAs<T, Decimal>) {
            w(reinterpret_cast<const U8*>(&src.scale), sizeof(src.scale));
            U8 negative = src.unscaled.negative ? 1 : 0;
            w(reinterpret_cast<const U8*>(&negative), sizeof(negative));
            w(reinterpret_cast<const U8*>(&src.unscaled.magnitude.length), sizeof(src.unscaled.magnitude.length));
            w(src.unscaled.magnitude.ptr, src.unscaled.magnitude.length);
        } else if constexpr (SameAs<T, Duration>) {
            w(reinterpret_cast<const U8*>(&src.months), sizeof(src.months));
            w(reinterpret_cast<const U8*>(&src.days), sizeof(src.days));
            w(reinterpret_cast<const U8*>(&src.nanoseconds), sizeof(src.nanoseconds));
        } else {
            static_assert(!SameAs<T,T>, "missing type case");
        }
    }

    void write_column_value(const Write auto& w, const ColumnValue& value, const type::Type& cdtype) {
        assert_true(can_write_column_value(value, cdtype), "invalid type to write for cdtype " + to_str(cdtype));
        visit(value, [&cdtype, &w](auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                assert_true(type_matches_tag<type::Basic>(cdtype.value), "static value type requires ctype basic, this should never happen");

                write_typed_basic(w, v, get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                bool is_vec = type_matches_tag<type::Vector>(cdtype.value);
                assert_true(type_matches_tag<type::List>(cdtype.value) || is_vec, "nested array requires ctype list/vector");
                if (!is_vec) {
                    U64 len = v.length;
                    w(reinterpret_cast<const U8*>(&len), sizeof(len));
                }
                const auto& elem = is_vec ? get<type::Vector>(cdtype.value).element : get<type::List>(cdtype.value).element;
                for (const auto& el : v)
                    write_column_value(w, el.value, elem);
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                assert_not_implemented("writing nested-element set is not implemented");
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                assert_not_implemented("writing nested-element map is not implemented");
            } else if constexpr (IsDynamicMap<T>) {
                assert_true(type_matches_tag<type::Map>(cdtype.value), "static value type requires ctype map, this should never happen");

                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                const auto& m = get<type::Map>(cdtype.value);
                for (auto it = v.begin(); it != v.end(); ++it) {
                    write_typed_basic(w, (*it).first, get<type::Basic>(m.key.value));
                    write_typed_basic(w, (*it).second, get<type::Basic>(m.value.value));
                }
            } else if constexpr (IsDynamicSet<T>) {
                assert_true(type_matches_tag<type::Set>(cdtype.value), "static value type requires ctype set, this should never happen");

                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (auto it = v.begin(); it != v.end(); ++it)
                    write_typed_basic(w, *it, get<type::Basic>(get<type::Set>(cdtype.value).key.value));
            } else if constexpr (IsDynamicArray<T>) {
                bool is_vec = type_matches_tag<type::Vector>(cdtype.value);
                assert_true(type_matches_tag<type::List>(cdtype.value) || is_vec, "static value type requires ctype list/vector, this should never happen");

                if (!is_vec) {
                    w(reinterpret_cast<const U8*>(&v.length), sizeof(v.length));
                }
                const auto element_basic = is_vec ? get<type::Basic>(get<type::Vector>(cdtype.value).element.value) : get<type::Basic>(get<type::List>(cdtype.value).element.value);
                for (const auto& el: v)
                    write_typed_basic(w, el, element_basic);
            } else if constexpr (SameAs<T, Null>) {
                // null column values are tracked via column mask; nothing to write
            } else {
                static_assert(!SameAs<T,T>, "unhandled read value type");
            }
        });
    }

    void write_column_mask(const Write auto& w, const IsColumnActive auto& is_active, U64 column_count) {
        w(reinterpret_cast<const U8*>(&column_count), sizeof(column_count));
        static_assert(sizeof(column_count) == COLUMN_COUNT_BYTE_COUNT);

        U64 mask = 0;
        for (U64 idx = 0; idx < column_count; idx++) {
            mask |= (static_cast<U64>(is_active(idx)) << (idx % MASK_BIT_COUNT));
            if (idx % MASK_BIT_COUNT == MASK_BIT_COUNT - 1) {
                w(reinterpret_cast<const U8*>(&mask), sizeof(mask));
                mask = 0;
            }
        }
        if (column_count % MASK_BIT_COUNT != 0) {
            w(reinterpret_cast<const U8*>(&mask), sizeof(mask));
        }
    }
}

export namespace plexdb {
    AutoString8 to_str(const cql::ColumnValue& value, [[maybe_unused]] cql::type::Basic dtype) {
        return visit(value, [](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (Either<T, AutoString8, S64, S32, S16, U8, F64, F32>) {
                return to_str(v);
            } else if constexpr (SameAs<T, cql::Null>) {
                return "null"_as;
            } else if constexpr (SameAs<T, cql::UUID>) {
                // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
                AutoString8 hex = bytes_to_hex(&v.value[0], 16);
                AutoString8 result{36};
                const char* h = hex.c_str;
                char* o = result.c_str;
                for (int i = 0; i < 8;  i++) o[i]    = h[i];
                o[8] = '-';
                for (int i = 0; i < 4;  i++) o[9+i]  = h[8+i];
                o[13] = '-';
                for (int i = 0; i < 4;  i++) o[14+i] = h[12+i];
                o[18] = '-';
                for (int i = 0; i < 4;  i++) o[19+i] = h[16+i];
                o[23] = '-';
                for (int i = 0; i < 12; i++) o[24+i] = h[20+i];
                return result;
            } else if constexpr (SameAs<T, cql::Blob>) {
                return "0x"_as + bytes_to_hex(v.value.ptr, v.value.length);
            } else if constexpr (SameAs<T, cql::Inet>) {
                if (v.is_v6) {
                    AutoString8 result = ""_as;
                    for (int i = 0; i < 8; i++) {
                        if (i > 0) result = result + ":";
                        result = result + bytes_to_hex(&v.v6[i*2], 2);
                    }
                    return result;
                } else {
                    return to_str(static_cast<U64>(v.v4[0])) + "." +
                           to_str(static_cast<U64>(v.v4[1])) + "." +
                           to_str(static_cast<U64>(v.v4[2])) + "." +
                           to_str(static_cast<U64>(v.v4[3]));
                }
            } else if constexpr (SameAs<T, cql::VarInt>) {
                AutoString8 prefix = v.negative ? "-0x"_as : "0x"_as;
                return prefix + bytes_to_hex(v.magnitude.ptr, v.magnitude.length);
            } else if constexpr (SameAs<T, cql::Decimal>) {
                AutoString8 prefix = v.unscaled.negative ? "-0x"_as : "0x"_as;
                return prefix + bytes_to_hex(v.unscaled.magnitude.ptr, v.unscaled.magnitude.length) +
                       "e-" + to_str(static_cast<S64>(v.scale));
            } else if constexpr (SameAs<T, cql::Duration>) {
                return to_str(static_cast<S64>(v.months)) + "mo" +
                       to_str(static_cast<S64>(v.days)) + "d" +
                       to_str(v.nanoseconds) + "ns";
            }
            assert_not_implemented("to_str for collection ColumnValue types is not implemented");
            return ""_as;
        });
    }

    AutoString8 to_str(const cql::ColumnValue& value, const cql::type::Type& cdtype) {
        if (type_matches_tag<cql::type::Basic>(cdtype.value))
            return to_str(value, get<cql::type::Basic>(cdtype.value));
        return visit(value, [&cdtype](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, DynamicArray<cql::NestedColumnValue>>) {
                bool is_vec = type_matches_tag<cql::type::Vector>(cdtype.value);
                const auto& elem = is_vec ? get<cql::type::Vector>(cdtype.value).element : get<cql::type::List>(cdtype.value).element;
                AutoString8 result = "["_as;
                bool first = true;
                for (const auto& el : v) {
                    if (!first) result = result + ", ";
                    result = result + to_str(el.value, elem);
                    first = false;
                }
                return result + "]";
            } else if constexpr (SameAs<T, DynamicSet<cql::NestedColumnValue>>) {
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str((*it).value, get<cql::type::Set>(cdtype.value).key);
                    first = false;
                }
                return result + "}";
            } else if constexpr (SameAs<T, DynamicMap<cql::NestedColumnValue, cql::NestedColumnValue>>) {
                const auto& m = get<cql::type::Map>(cdtype.value);
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str((*it).first.value, m.key);
                    result = result + ": ";
                    result = result + to_str((*it).second.value, m.value);
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicMap<T>) {
                const auto& m = get<cql::type::Map>(cdtype.value);
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str(cql::ColumnValue{(*it).first}, get<cql::type::Basic>(m.key.value));
                    result = result + ": ";
                    result = result + to_str(cql::ColumnValue{(*it).second}, get<cql::type::Basic>(m.value.value));
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicSet<T>) {
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str(cql::ColumnValue{*it}, get<cql::type::Basic>(get<cql::type::Set>(cdtype.value).key.value));
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicArray<T>) {
                bool is_vec = type_matches_tag<cql::type::Vector>(cdtype.value);
                const auto& elem = is_vec ? get<cql::type::Vector>(cdtype.value).element : get<cql::type::List>(cdtype.value).element;
                AutoString8 result = "["_as;
                bool first = true;
                for (const auto& el : v) {
                    if (!first) result = result + ", ";
                    result = result + to_str(cql::ColumnValue{el}, get<cql::type::Basic>(elem.value));
                    first = false;
                }
                return result + "]";
            } else {
                return to_str(cql::ColumnValue{v}, get<cql::type::Basic>(cdtype.value));
            }
        });
    }
}
