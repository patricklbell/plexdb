module;
#include <coroutine>

export module cql.engine.io;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;

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
    coroutine::Task<ColumnValue> read_column_value(const Read auto& r, BasicType dtype) {
        switch (dtype) {
            case BasicType::text:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));

                AutoString8 value{length};
                co_await r(reinterpret_cast<U8*>(value.c_str), length);

                co_return {move(value)};
            }break;
            case BasicType::smallint:{
                S16 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::int_:{
                S32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::counter:
            case BasicType::timestamp:
            case BasicType::bigint:{
                S64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::boolean:{
                U8 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::float_:{
                F32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::double_:{
                F64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::uuid:{
                UUID value;
                co_await r(&value.value[0], value.length);

                co_return {move(value)};
            }break;
            case BasicType::blob:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));

                Blob value;
                resize(value.value, length);
                assert_true(value.value.length == length, "resize successful");
                co_await r(value.value.ptr, length);

                co_return {move(value)};
            }
            case BasicType::ascii:
            case BasicType::varchar:{
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(length));

                AutoString8 value{length};
                co_await r(reinterpret_cast<U8*>(value.c_str), length);

                co_return {move(value)};
            }break;
            case BasicType::tinyint:{
                U8 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::date:{
                S32 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::time:{
                S64 value;
                co_await r(reinterpret_cast<U8*>(&value), sizeof(value));

                co_return {move(value)};
            }break;
            case BasicType::timeuuid:{
                UUID value;
                co_await r(&value.value[0], value.length);

                co_return {move(value)};
            }break;
            case BasicType::inet:{
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
            case BasicType::varint:{
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
            case BasicType::decimal:{
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
            case BasicType::duration:{
                Duration value;
                co_await r(reinterpret_cast<U8*>(&value.months), sizeof(value.months));
                co_await r(reinterpret_cast<U8*>(&value.days), sizeof(value.days));
                co_await r(reinterpret_cast<U8*>(&value.nanoseconds), sizeof(value.nanoseconds));
                co_return {move(value)};
            }break;
            case BasicType::hex:{
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

    coroutine::Task<ColumnValue> read_column_value(const Read auto& r, const Type& cdtype) {
        if (type_matches_tag<Type::Basic>(cdtype.variants)) {
            co_return co_await read_column_value(r, get<Type::Basic>(cdtype.variants).value_dtype);
        }

        if (type_matches_tag<Type::List>(cdtype.variants)) {
            auto& v = get<Type::List>(cdtype.variants);
            U64 length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            if (type_matches_tag<Type>(v.element)) {
                DynamicArray<NestedColumnValue> arr{};
                for (U64 i = 0; i < length; i++) {
                    NestedColumnValue nv;
                    nv.value = co_await read_column_value(r, get<Type>(v.element));
                    push_back(arr, move(nv));
                }
                co_return {move(arr)};
            }
            auto read_arr = [&]<typename El>() -> coroutine::Task<ColumnValue> {
                DynamicArray<El> arr{};
                for (U64 i = 0; i < length; i++)
                    push_back(arr, get<El>(co_await read_column_value(r, get<BasicType>(v.element))));
                co_return {move(arr)};
            };
            switch (get<BasicType>(v.element)) {
                case BasicType::text:       co_return co_await read_arr.template operator()<AutoString8>();
                case BasicType::smallint:   co_return co_await read_arr.template operator()<S16>();
                case BasicType::int_:       co_return co_await read_arr.template operator()<S32>();
                case BasicType::bigint:
                case BasicType::timestamp:
                case BasicType::counter:
                case BasicType::time:       co_return co_await read_arr.template operator()<S64>();
                case BasicType::boolean:    co_return co_await read_arr.template operator()<U8>();
                case BasicType::float_:     co_return co_await read_arr.template operator()<F32>();
                case BasicType::double_:    co_return co_await read_arr.template operator()<F64>();
                case BasicType::uuid:
                case BasicType::timeuuid:   co_return co_await read_arr.template operator()<UUID>();
                case BasicType::tinyint:    co_return co_await read_arr.template operator()<U8>();
                case BasicType::date:       co_return co_await read_arr.template operator()<S32>();
                case BasicType::ascii:
                case BasicType::varchar:    co_return co_await read_arr.template operator()<AutoString8>();
                case BasicType::blob:       co_return co_await read_arr.template operator()<Blob>();
                case BasicType::hex:        co_return co_await read_arr.template operator()<Blob>();
                case BasicType::inet:       co_return co_await read_arr.template operator()<Inet>();
                case BasicType::varint:     co_return co_await read_arr.template operator()<VarInt>();
                case BasicType::decimal:    co_return co_await read_arr.template operator()<Decimal>();
                case BasicType::duration:   co_return co_await read_arr.template operator()<Duration>();
            }
            assert_true(false, "invalid basic type for list read");
            co_return {};
        }

        if (type_matches_tag<Type::Set>(cdtype.variants)) {
            auto& v = get<Type::Set>(cdtype.variants);
            U64 length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            if (type_matches_tag<Type>(v.key)) {
                assert_not_implemented("reading nested-element set is not implemented");
                co_return {};
            }
            auto read_set = [&]<typename K>() -> coroutine::Task<ColumnValue> {
                DynamicSet<K> s{};
                for (U64 i = 0; i < length; i++)
                    insert(s, get<K>(co_await read_column_value(r, get<BasicType>(v.key))));
                co_return {move(s)};
            };
            switch (get<BasicType>(v.key)) {
                case BasicType::text:       co_return co_await read_set.template operator()<AutoString8>();
                case BasicType::smallint:   co_return co_await read_set.template operator()<S16>();
                case BasicType::int_:       co_return co_await read_set.template operator()<S32>();
                case BasicType::bigint:
                case BasicType::timestamp:
                case BasicType::counter:
                case BasicType::time:       co_return co_await read_set.template operator()<S64>();
                case BasicType::boolean:    co_return co_await read_set.template operator()<U8>();
                case BasicType::float_:     co_return co_await read_set.template operator()<F32>();
                case BasicType::double_:    co_return co_await read_set.template operator()<F64>();
                case BasicType::uuid:
                case BasicType::timeuuid:   co_return co_await read_set.template operator()<UUID>();
                case BasicType::tinyint:    co_return co_await read_set.template operator()<U8>();
                case BasicType::date:       co_return co_await read_set.template operator()<S32>();
                case BasicType::ascii:
                case BasicType::varchar:    co_return co_await read_set.template operator()<AutoString8>();
                case BasicType::blob:       co_return co_await read_set.template operator()<Blob>();
                case BasicType::hex:        co_return co_await read_set.template operator()<Blob>();
                case BasicType::inet:       co_return co_await read_set.template operator()<Inet>();
                case BasicType::varint:     co_return co_await read_set.template operator()<VarInt>();
                case BasicType::decimal:    co_return co_await read_set.template operator()<Decimal>();
                case BasicType::duration:   co_return co_await read_set.template operator()<Duration>();
            }
            assert_true(false, "invalid basic type for set read");
            co_return {};
        }

        if (type_matches_tag<Type::Map>(cdtype.variants)) {
            auto& v = get<Type::Map>(cdtype.variants);
            U64 length;
            co_await r(reinterpret_cast<U8*>(&length), sizeof(length));
            if (type_matches_tag<Type>(v.key) || type_matches_tag<Type>(v.value)) {
                assert_not_implemented("reading nested-element map is not implemented");
                co_return {};
            }
            auto read_map_kv = [&]<typename K, typename V>() -> coroutine::Task<ColumnValue> {
                DynamicMap<K, V> m{};
                for (U64 i = 0; i < length; i++) {
                    K k = get<K>(co_await read_column_value(r, get<BasicType>(v.key)));
                    V val = get<V>(co_await read_column_value(r, get<BasicType>(v.value)));
                    insert(m, move(k), move(val));
                }
                co_return {move(m)};
            };
            auto read_map_v = [&]<typename K>(BasicType val_dtype) -> coroutine::Task<ColumnValue> {
                switch (val_dtype) {
                    case BasicType::text:       co_return co_await read_map_kv.template operator()<K, AutoString8>();
                    case BasicType::smallint:   co_return co_await read_map_kv.template operator()<K, S16>();
                    case BasicType::int_:       co_return co_await read_map_kv.template operator()<K, S32>();
                    case BasicType::bigint:
                    case BasicType::timestamp:
                    case BasicType::counter:
                    case BasicType::time:       co_return co_await read_map_kv.template operator()<K, S64>();
                    case BasicType::boolean:    co_return co_await read_map_kv.template operator()<K, U8>();
                    case BasicType::float_:     co_return co_await read_map_kv.template operator()<K, F32>();
                    case BasicType::double_:    co_return co_await read_map_kv.template operator()<K, F64>();
                    case BasicType::uuid:
                    case BasicType::timeuuid:   co_return co_await read_map_kv.template operator()<K, UUID>();
                    case BasicType::tinyint:    co_return co_await read_map_kv.template operator()<K, U8>();
                    case BasicType::date:       co_return co_await read_map_kv.template operator()<K, S32>();
                    case BasicType::ascii:
                    case BasicType::varchar:    co_return co_await read_map_kv.template operator()<K, AutoString8>();
                    case BasicType::blob:       co_return co_await read_map_kv.template operator()<K, Blob>();
                    case BasicType::hex:        co_return co_await read_map_kv.template operator()<K, Blob>();
                    case BasicType::inet:       co_return co_await read_map_kv.template operator()<K, Inet>();
                    case BasicType::varint:     co_return co_await read_map_kv.template operator()<K, VarInt>();
                    case BasicType::decimal:    co_return co_await read_map_kv.template operator()<K, Decimal>();
                    case BasicType::duration:   co_return co_await read_map_kv.template operator()<K, Duration>();
                }
                assert_true(false, "invalid basic type for map value read");
                co_return {};
            };
            BasicType map_val_bt = get<BasicType>(v.value);
            switch (get<BasicType>(v.key)) {
                case BasicType::text:       co_return co_await read_map_v.template operator()<AutoString8>(map_val_bt);
                case BasicType::smallint:   co_return co_await read_map_v.template operator()<S16>(map_val_bt);
                case BasicType::int_:       co_return co_await read_map_v.template operator()<S32>(map_val_bt);
                case BasicType::bigint:
                case BasicType::timestamp:
                case BasicType::counter:
                case BasicType::time:       co_return co_await read_map_v.template operator()<S64>(map_val_bt);
                case BasicType::boolean:    co_return co_await read_map_v.template operator()<U8>(map_val_bt);
                case BasicType::float_:     co_return co_await read_map_v.template operator()<F32>(map_val_bt);
                case BasicType::double_:    co_return co_await read_map_v.template operator()<F64>(map_val_bt);
                case BasicType::uuid:
                case BasicType::timeuuid:   co_return co_await read_map_v.template operator()<UUID>(map_val_bt);
                case BasicType::tinyint:    co_return co_await read_map_v.template operator()<U8>(map_val_bt);
                case BasicType::date:       co_return co_await read_map_v.template operator()<S32>(map_val_bt);
                case BasicType::ascii:
                case BasicType::varchar:    co_return co_await read_map_v.template operator()<AutoString8>(map_val_bt);
                case BasicType::blob:       co_return co_await read_map_v.template operator()<Blob>(map_val_bt);
                case BasicType::hex:        co_return co_await read_map_v.template operator()<Blob>(map_val_bt);
                case BasicType::inet:       co_return co_await read_map_v.template operator()<Inet>(map_val_bt);
                case BasicType::varint:     co_return co_await read_map_v.template operator()<VarInt>(map_val_bt);
                case BasicType::decimal:    co_return co_await read_map_v.template operator()<Decimal>(map_val_bt);
                case BasicType::duration:   co_return co_await read_map_v.template operator()<Duration>(map_val_bt);
            }
            assert_true(false, "invalid basic type for map key read");
            co_return {};
        }

        // Vector
        {
            auto& v = get<Type::Vector>(cdtype.variants);
            U64 count = v.count;
            if (type_matches_tag<Type>(v.element)) {
                DynamicArray<NestedColumnValue> arr{};
                for (U64 i = 0; i < count; i++) {
                    NestedColumnValue nv;
                    nv.value = co_await read_column_value(r, get<Type>(v.element));
                    push_back(arr, move(nv));
                }
                co_return {move(arr)};
            }
            auto read_vec = [&]<typename El>() -> coroutine::Task<ColumnValue> {
                DynamicArray<El> arr{};
                for (U64 i = 0; i < count; i++)
                    push_back(arr, get<El>(co_await read_column_value(r, get<BasicType>(v.element))));
                co_return {move(arr)};
            };
            switch (get<BasicType>(v.element)) {
                case BasicType::text:       co_return co_await read_vec.template operator()<AutoString8>();
                case BasicType::smallint:   co_return co_await read_vec.template operator()<S16>();
                case BasicType::int_:       co_return co_await read_vec.template operator()<S32>();
                case BasicType::bigint:
                case BasicType::timestamp:
                case BasicType::counter:
                case BasicType::time:       co_return co_await read_vec.template operator()<S64>();
                case BasicType::boolean:    co_return co_await read_vec.template operator()<U8>();
                case BasicType::float_:     co_return co_await read_vec.template operator()<F32>();
                case BasicType::double_:    co_return co_await read_vec.template operator()<F64>();
                case BasicType::uuid:
                case BasicType::timeuuid:   co_return co_await read_vec.template operator()<UUID>();
                case BasicType::tinyint:    co_return co_await read_vec.template operator()<U8>();
                case BasicType::date:       co_return co_await read_vec.template operator()<S32>();
                case BasicType::ascii:
                case BasicType::varchar:    co_return co_await read_vec.template operator()<AutoString8>();
                case BasicType::blob:       co_return co_await read_vec.template operator()<Blob>();
                case BasicType::hex:        co_return co_await read_vec.template operator()<Blob>();
                case BasicType::inet:       co_return co_await read_vec.template operator()<Inet>();
                case BasicType::varint:     co_return co_await read_vec.template operator()<VarInt>();
                case BasicType::decimal:    co_return co_await read_vec.template operator()<Decimal>();
                case BasicType::duration:   co_return co_await read_vec.template operator()<Duration>();
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
    coroutine::Task<void> skip_column_value(const Read auto& r, BasicType dtype) {
        switch (dtype) {
            case BasicType::boolean:
            case BasicType::tinyint:   co_await r(nullptr, 1);             co_return;
            case BasicType::smallint:  co_await r(nullptr, sizeof(S16));   co_return;
            case BasicType::int_:
            case BasicType::date:
            case BasicType::float_:    co_await r(nullptr, sizeof(S32));   co_return;
            case BasicType::bigint:
            case BasicType::counter:
            case BasicType::timestamp:
            case BasicType::time:
            case BasicType::double_:   co_await r(nullptr, sizeof(S64));   co_return;
            case BasicType::uuid:
            case BasicType::timeuuid:  co_await r(nullptr, 16);            co_return;
            case BasicType::duration:  co_await r(nullptr, 4+4+8);         co_return;
            case BasicType::text:
            case BasicType::ascii:
            case BasicType::varchar:
            case BasicType::blob:
            case BasicType::hex: {
                U64 length;
                co_await r(reinterpret_cast<U8*>(&length), sizeof(U64));
                co_await r(nullptr, length);
                co_return;
            }
            case BasicType::varint: {
                co_await r(nullptr, 1);  // negative byte
                U64 mag_length;
                co_await r(reinterpret_cast<U8*>(&mag_length), sizeof(U64));
                co_await r(nullptr, mag_length);
                co_return;
            }
            case BasicType::decimal: {
                co_await r(nullptr, sizeof(S32) + 1);  // scale + negative byte
                U64 mag_length;
                co_await r(reinterpret_cast<U8*>(&mag_length), sizeof(U64));
                co_await r(nullptr, mag_length);
                co_return;
            }
            case BasicType::inet: {
                U8 is_v6;
                co_await r(&is_v6, 1);
                co_await r(nullptr, is_v6 ? 16_u64 : 4_u64);
                co_return;
            }
        }
        assert_true(false, "invalid basic type in skip_column_value");
    }

    coroutine::Task<void> skip_column_value(const Read auto& r, const Type& col_type) {
        if (type_matches_tag<Type::Basic>(col_type.variants)) {
            co_await skip_column_value(r, get<Type::Basic>(col_type.variants).value_dtype);
            co_return;
        }

        U64 count = 0;
        BasicType elem_bt{};
        bool elem_is_type = false;
        const Type* elem_type_ptr = nullptr;

        if (type_matches_tag<Type::List>(col_type.variants)) {
            auto& lv = get<Type::List>(col_type.variants);
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            elem_is_type = type_matches_tag<Type>(lv.element);
            if (elem_is_type) elem_type_ptr = &get<Type>(lv.element);
            else              elem_bt = get<BasicType>(lv.element);
        } else if (type_matches_tag<Type::Set>(col_type.variants)) {
            auto& sv = get<Type::Set>(col_type.variants);
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            elem_is_type = type_matches_tag<Type>(sv.key);
            if (elem_is_type) elem_type_ptr = &get<Type>(sv.key);
            else              elem_bt = get<BasicType>(sv.key);
        } else if (type_matches_tag<Type::Map>(col_type.variants)) {
            auto& mv = get<Type::Map>(col_type.variants);
            co_await r(reinterpret_cast<U8*>(&count), sizeof(count));
            bool key_is_type = type_matches_tag<Type>(mv.key);
            bool val_is_type = type_matches_tag<Type>(mv.value);
            BasicType kbt = key_is_type ? BasicType{} : get<BasicType>(mv.key);
            BasicType vbt = val_is_type ? BasicType{} : get<BasicType>(mv.value);
            for (U64 i = 0; i < count; i++) {
                if (key_is_type) co_await skip_column_value(r, get<Type>(mv.key));
                else             co_await skip_column_value(r, kbt);
                if (val_is_type) co_await skip_column_value(r, get<Type>(mv.value));
                else             co_await skip_column_value(r, vbt);
            }
            co_return;
        } else if (type_matches_tag<Type::Vector>(col_type.variants)) {
            auto& vv = get<Type::Vector>(col_type.variants);
            count = vv.count;
            elem_is_type = type_matches_tag<Type>(vv.element);
            if (elem_is_type) elem_type_ptr = &get<Type>(vv.element);
            else              elem_bt = get<BasicType>(vv.element);
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
    bool can_cast_write_typed_basic_as_column_value(BasicType dtype) {
        using TT = RemoveCV<T>;
        if constexpr (Either<TT, String8, AutoString8>) {
            return dtype == BasicType::text || dtype == BasicType::timestamp || dtype == BasicType::uuid ||
                   dtype == BasicType::ascii || dtype == BasicType::varchar ||
                   dtype == BasicType::inet || dtype == BasicType::varint || dtype == BasicType::decimal ||
                   dtype == BasicType::duration || dtype == BasicType::hex;
        } else if constexpr (Either<TT, S64>) {
            return dtype == BasicType::int_ || dtype == BasicType::bigint || dtype == BasicType::smallint ||
                   dtype == BasicType::counter || dtype == BasicType::tinyint || dtype == BasicType::date || dtype == BasicType::time;
        } else if constexpr (Either<TT, bool>) {
            return dtype == BasicType::boolean;
        } else if constexpr (Either<TT, F64, F32>) {
            return dtype == BasicType::float_ || dtype == BasicType::double_;
        } else if constexpr (SameAs<TT, Hex>) {
            return dtype == BasicType::hex;
        } else if constexpr (SameAs<TT, UUID>) {
            return dtype == BasicType::uuid || dtype == BasicType::timeuuid;
        } else if constexpr (SameAs<TT, Blob>) {
            return dtype == BasicType::blob || dtype == BasicType::inet || dtype == BasicType::varint ||
                   dtype == BasicType::decimal || dtype == BasicType::duration ||
                   dtype == BasicType::hex;
        } else if constexpr (SameAs<TT, Null>) {
            return true;
        } else {
            static_assert(!SameAs<T,T>, "missing type case");
            return false;
        }
    }

    template<typename T>
    bool can_write_typed_basic_as_column_value(BasicType dtype) {
        if constexpr (Either<RemoveCV<T>, AutoString8>)   { return dtype == BasicType::text || dtype == BasicType::ascii || dtype == BasicType::varchar; }
        else if constexpr (Either<RemoveCV<T>, S16>)      { return dtype == BasicType::smallint; }
        else if constexpr (Either<RemoveCV<T>, S32>)      { return dtype == BasicType::int_ || dtype == BasicType::date; }
        else if constexpr (Either<RemoveCV<T>, S64>)      { return dtype == BasicType::bigint || dtype == BasicType::timestamp || dtype == BasicType::counter || dtype == BasicType::time; }
        else if constexpr (Either<RemoveCV<T>, U8>)       { return dtype == BasicType::boolean || dtype == BasicType::tinyint; }
        else if constexpr (Either<RemoveCV<T>, F32>)      { return dtype == BasicType::float_; }
        else if constexpr (Either<RemoveCV<T>, F64>)      { return dtype == BasicType::double_; }
        else if constexpr (Either<RemoveCV<T>, UUID>)     { return dtype == BasicType::uuid || dtype == BasicType::timeuuid; }
        else if constexpr (Either<RemoveCV<T>, Null>)     { return true; }
        else if constexpr (Either<RemoveCV<T>, Blob>)     { return dtype == BasicType::blob || dtype == BasicType::hex; }
        else if constexpr (Either<RemoveCV<T>, Inet>)     { return dtype == BasicType::inet; }
        else if constexpr (Either<RemoveCV<T>, VarInt>)   { return dtype == BasicType::varint; }
        else if constexpr (Either<RemoveCV<T>, Decimal>)  { return dtype == BasicType::decimal; }
        else if constexpr (Either<RemoveCV<T>, Duration>) { return dtype == BasicType::duration; }
        else                                              { static_assert(!SameAs<T,T>, "missing type case"); return false; }
    }

    bool can_cast_write_constant_as_column_value(const Constant& constant, BasicType dtype) {
        return visit(constant.value, [&](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            return can_cast_write_typed_basic_as_column_value<T>(dtype);
        });
    }

    bool can_write_column_value(const ColumnValue& value, const Type& cdtype);

    bool can_cast_write_evaluated_as_column_value(const Evaluated& evaluated, const Type& cdtype) {
        return visit(evaluated.value, [&cdtype](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                return type_matches_tag<Type::Basic>(cdtype.variants) &&
                       can_cast_write_constant_as_column_value(cv, get<Type::Basic>(cdtype.variants).value_dtype);
            } else if constexpr (SameAs<T, MapLiteral>) {
                return type_matches_tag<Type::Map>(cdtype.variants);
            } else if constexpr (SameAs<T, SetLiteral>) {
                return type_matches_tag<Type::Set>(cdtype.variants);
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                return type_matches_tag<Type::List>(cdtype.variants) || type_matches_tag<Type::Vector>(cdtype.variants);
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
    void write_default_column_value(const Write auto& w, BasicType dtype) {
        switch (dtype) {
            case BasicType::text:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
            case BasicType::smallint:{
                S16 smallint = 0_s16;
                w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
            }break;
            case BasicType::int_:{
                S32 int_ = 0_s32;
                w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
            }break;
            case BasicType::counter:
            case BasicType::bigint:{
                S64 bigint = 0_s64;
                w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
            }break;
            case BasicType::boolean:{
                U8 boolean = false;
                w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
            }break;
            case BasicType::float_:{
                F32 float_ = 0_f32;
                w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
            }break;
            case BasicType::double_:{
                F64 double_ = 0_f64;
                w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
            }break;
            case BasicType::uuid:{
                UUID uuid{};
                w(&uuid.value[0], uuid.value.length);
            }break;
            case BasicType::blob:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
            case BasicType::timestamp:
            case BasicType::time:
            case BasicType::ascii:
            case BasicType::varchar:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
            case BasicType::tinyint:{
                U8 tinyint = U8(0);
                w(reinterpret_cast<const U8*>(&tinyint), sizeof(tinyint));
            }break;
            case BasicType::date:{
                S32 date = 0_s32;
                w(reinterpret_cast<const U8*>(&date), sizeof(date));
            }break;
            case BasicType::timeuuid:{
                UUID uuid{};
                w(&uuid.value[0], uuid.length);
            }break;
            case BasicType::inet:{
                U8 is_v6 = 0;
                w(reinterpret_cast<const U8*>(&is_v6), sizeof(is_v6));
                U8 zeros[4] = {0, 0, 0, 0};
                w(zeros, 4);
            }break;
            case BasicType::varint:{
                U8 negative = 0;
                w(reinterpret_cast<const U8*>(&negative), sizeof(negative));
                U64 mag_length = 0;
                w(reinterpret_cast<const U8*>(&mag_length), sizeof(mag_length));
            }break;
            case BasicType::decimal:{
                S32 scale = 0;
                w(reinterpret_cast<const U8*>(&scale), sizeof(scale));
                U8 negative = 0;
                w(reinterpret_cast<const U8*>(&negative), sizeof(negative));
                U64 mag_length = 0;
                w(reinterpret_cast<const U8*>(&mag_length), sizeof(mag_length));
            }break;
            case BasicType::duration:{
                S32 zero32 = 0;
                S64 zero64 = 0;
                w(reinterpret_cast<const U8*>(&zero32), sizeof(zero32));
                w(reinterpret_cast<const U8*>(&zero32), sizeof(zero32));
                w(reinterpret_cast<const U8*>(&zero64), sizeof(zero64));
            }break;
            case BasicType::hex:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
        }
    }

    void write_default_column_value(const Write auto& w, const Type& cdtype) {
        visit(cdtype.variants, [&w](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, Type::Basic>) {
                write_default_column_value(w, v.value_dtype);
            } else {
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }
        });
    }

    template<typename T>
    void write_typed_basic_as_column_value(const Write auto& w, const T& src, BasicType dtype) {
        if constexpr (SameAs<T, AutoString8>) {
            switch (dtype) {
                case BasicType::text:
                case BasicType::ascii:
                case BasicType::varchar:{
                    w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                    w(reinterpret_cast<const U8*>(src.c_str), src.length);
                }break;
                case BasicType::inet:
                case BasicType::varint:
                case BasicType::decimal:
                case BasicType::duration:
                    case BasicType::hex:{
                    w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                    w(reinterpret_cast<const U8*>(src.c_str), src.length);
                }break;
                default:{ assert_not_implemented("writing string-like value to this dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, S64>) {
            switch (dtype) {
                case BasicType::tinyint:{
                    U8 tinyint = static_cast<U8>(static_cast<S8>(src));
                    w(reinterpret_cast<const U8*>(&tinyint), sizeof(tinyint));
                }break;
                case BasicType::smallint:{
                    S16 smallint = static_cast<S16>(src);
                    w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                }break;
                case BasicType::int_:{
                    S32 int_ = static_cast<S32>(src);
                    w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                }break;
                case BasicType::date:{
                    S32 date = static_cast<S32>(src);
                    w(reinterpret_cast<const U8*>(&date), sizeof(date));
                }break;
                case BasicType::time:
                case BasicType::timestamp:
                case BasicType::counter:
                case BasicType::bigint:{
                    S64 bigint = static_cast<S64>(src);
                    w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                }break;
                default:{ assert_not_implemented("writing integer value to this dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, bool>) {
            switch (dtype) {
                case BasicType::boolean:{
                    U8 boolean = static_cast<U8>(src);
                    w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                }break;
                default:{ assert_not_implemented("writing boolean value to non-boolean dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, F64>) {
            switch (dtype) {
                case BasicType::float_:{
                    F32 float_ = static_cast<F32>(src);
                    w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                }break;
                case BasicType::double_:{
                    F64 double_ = static_cast<F64>(src);
                    w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                }break;
                default:{ assert_not_implemented("writing float value to non-float dtype is not implemented"); }break;
            }
        } else if constexpr (SameAs<T, UUID>) {
            assert_true(dtype == BasicType::uuid || dtype == BasicType::timeuuid, "UUID value written to non-uuid column");
            w(&src.value[0], src.length);
        } else if constexpr (SameAs<T, Hex>) {
            assert_true(dtype == BasicType::hex, "hex value written to non-hex column");
            w(reinterpret_cast<const U8*>(&src.value.length), sizeof(src.value.length));
            w(&src.value[0], src.value.length);
        } else if constexpr (SameAs<T, Blob>) {
            assert_true(dtype == BasicType::blob || dtype == BasicType::inet || dtype == BasicType::varint ||
                        dtype == BasicType::decimal || dtype == BasicType::duration ||
                        dtype == BasicType::hex, "blob value written to incompatible column dtype");
            w(reinterpret_cast<const U8*>(&src.value.length), sizeof(src.value.length));
            w(&src.value[0], src.value.length);
        } else if constexpr (SameAs<T, Null>) {
            assert_not_implemented("writing null column values is not implemented");
        } else {
            static_assert(!SameAs<T,T>, "missing underlying type case");
        }
    }

    void cast_write_constant_as_column_value(const Write auto& w, const Constant& constant, BasicType dtype) {
        visit(constant.value, [&](const auto& cv) {
            using T = Decay<decltype(cv)>;
            write_typed_basic_as_column_value<T>(w, cv, dtype);
        });
    }

    inline Type element_type_to_type(const ElementType& e) {
        if (type_matches_tag<BasicType>(e)) return create_basic(get<BasicType>(e));
        return get<Type>(e);
    }

    void write_column_value(const Write auto& w, const ColumnValue& value, const Type& cdtype);

    void cast_write_evaluated_as_column_value(const Write auto& w, const Evaluated& evaluated, const Type& cdtype) {
        assert_true(can_cast_write_evaluated_as_column_value(evaluated, cdtype), "invalid evaluated value for write");

        visit(evaluated.value, [&](const auto& cv) {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                assert_true(type_matches_tag<Type::Basic>(cdtype.variants), "bad can write check!");
                cast_write_constant_as_column_value(w, cv, get<Type::Basic>(cdtype.variants).value_dtype);
            } else if constexpr (SameAs<T, MapLiteral>) {
                assert_true(type_matches_tag<Type::Map>(cdtype.variants), "map literal written to non-map column");
                auto& m = get<Type::Map>(cdtype.variants);
                Type key_type = element_type_to_type(m.key);
                Type val_type = element_type_to_type(m.value);
                U64 len = cv.key_values.length;
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (U64 i = 0; i < len; i++) {
                    Evaluated k = evaluate(cv.key_values[i].first);
                    Evaluated v = evaluate(cv.key_values[i].second);
                    cast_write_evaluated_as_column_value(w, k, key_type);
                    cast_write_evaluated_as_column_value(w, v, val_type);
                }
            } else if constexpr (SameAs<T, SetLiteral>) {
                assert_true(type_matches_tag<Type::Set>(cdtype.variants), "set literal written to non-set column");
                Type key_type = element_type_to_type(get<Type::Set>(cdtype.variants).key);
                U64 len = cv.keys.length;
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (U64 i = 0; i < len; i++) {
                    Evaluated el = evaluate(cv.keys[i]);
                    cast_write_evaluated_as_column_value(w, el, key_type);
                }
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                bool is_vec = type_matches_tag<Type::Vector>(cdtype.variants);
                bool is_list = type_matches_tag<Type::List>(cdtype.variants);
                assert_true(is_list || is_vec, "list/vector literal written to incompatible column");
                if (is_list) {
                    U64 len = cv.elements.length;
                    w(reinterpret_cast<const U8*>(&len), sizeof(len));
                }
                Type el_type = element_type_to_type(
                    is_vec ? get<Type::Vector>(cdtype.variants).element : get<Type::List>(cdtype.variants).element);
                for (U64 i = 0; i < cv.elements.length; i++) {
                    Evaluated el = evaluate(cv.elements[i]);
                    cast_write_evaluated_as_column_value(w, el, el_type);
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

    bool can_write_column_value(const ColumnValue& value, const Type& cdtype) {
        return visit(value, [&cdtype](const auto& v) -> bool {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return type_matches_tag<Type::Basic>(cdtype.variants) &&
                       can_write_typed_basic_as_column_value<T>(get<Type::Basic>(cdtype.variants).value_dtype);
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                bool is_list = type_matches_tag<Type::List>(cdtype.variants);
                bool is_vec  = type_matches_tag<Type::Vector>(cdtype.variants);
                if (!is_list && !is_vec) return false;
                auto& elem = is_vec ? get<Type::Vector>(cdtype.variants).element : get<Type::List>(cdtype.variants).element;
                return type_matches_tag<Type>(elem);
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                return type_matches_tag<Type::Set>(cdtype.variants) &&
                       type_matches_tag<Type>(get<Type::Set>(cdtype.variants).key);
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                if (!type_matches_tag<Type::Map>(cdtype.variants)) return false;
                auto& m = get<Type::Map>(cdtype.variants);
                return type_matches_tag<Type>(m.key) && type_matches_tag<Type>(m.value);
            } else if constexpr (IsDynamicMap<T>) {
                if (!type_matches_tag<Type::Map>(cdtype.variants)) return false;
                auto& m = get<Type::Map>(cdtype.variants);
                return type_matches_tag<BasicType>(m.key) && type_matches_tag<BasicType>(m.value) &&
                       can_write_typed_basic_as_column_value<typename T::Key>(get<BasicType>(m.key)) &&
                       can_write_typed_basic_as_column_value<typename T::Value>(get<BasicType>(m.value));
            } else if constexpr (IsDynamicSet<T>) {
                if (!type_matches_tag<Type::Set>(cdtype.variants)) return false;
                auto& s = get<Type::Set>(cdtype.variants);
                return type_matches_tag<BasicType>(s.key) &&
                       can_write_typed_basic_as_column_value<typename T::Key>(get<BasicType>(s.key));
            } else if constexpr (IsDynamicArray<T>) {
                bool is_list = type_matches_tag<Type::List>(cdtype.variants);
                bool is_vec  = type_matches_tag<Type::Vector>(cdtype.variants);
                if (!is_list && !is_vec) return false;
                auto& elem = is_vec ? get<Type::Vector>(cdtype.variants).element : get<Type::List>(cdtype.variants).element;
                return type_matches_tag<BasicType>(elem) &&
                       can_write_typed_basic_as_column_value<typename T::Element>(get<BasicType>(elem));
            } else if constexpr (SameAs<T, Null>) {
                return true;
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
            }
        });
    }

    template<typename T>
    void write_typed_basic(const Write auto& w, const T& src, [[maybe_unused]] BasicType dtype) {
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

    void write_column_value(const Write auto& w, const ColumnValue& value, const Type& cdtype) {
        assert_true(can_write_column_value(value, cdtype), "invalid type to write for cdtype " + to_str(cdtype));
        visit(value, [&cdtype, &w](auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                assert_true(type_matches_tag<Type::Basic>(cdtype.variants), "static value type requires ctype basic, this should never happen");

                write_typed_basic(w, v, get<Type::Basic>(cdtype.variants).value_dtype);
            } else if constexpr (SameAs<T, DynamicArray<NestedColumnValue>>) {
                bool is_vec = type_matches_tag<Type::Vector>(cdtype.variants);
                assert_true(type_matches_tag<Type::List>(cdtype.variants) || is_vec, "nested array requires ctype list/vector");
                if (!is_vec) {
                    U64 len = v.length;
                    w(reinterpret_cast<const U8*>(&len), sizeof(len));
                }
                const auto& elem = is_vec ? get<Type::Vector>(cdtype.variants).element : get<Type::List>(cdtype.variants).element;
                for (const auto& el : v)
                    write_column_value(w, el.value, get<Type>(elem));
            } else if constexpr (SameAs<T, DynamicSet<NestedColumnValue>>) {
                assert_not_implemented("writing nested-element set is not implemented");
            } else if constexpr (SameAs<T, DynamicMap<NestedColumnValue, NestedColumnValue>>) {
                assert_not_implemented("writing nested-element map is not implemented");
            } else if constexpr (IsDynamicMap<T>) {
                assert_true(type_matches_tag<Type::Map>(cdtype.variants), "static value type requires ctype map, this should never happen");

                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                const auto& m = get<Type::Map>(cdtype.variants);
                for (auto it = v.begin(); it != v.end(); ++it) {
                    write_typed_basic(w, (*it).first, get<BasicType>(m.key));
                    write_typed_basic(w, (*it).second, get<BasicType>(m.value));
                }
            } else if constexpr (IsDynamicSet<T>) {
                assert_true(type_matches_tag<Type::Set>(cdtype.variants), "static value type requires ctype set, this should never happen");

                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (auto it = v.begin(); it != v.end(); ++it)
                    write_typed_basic(w, *it, get<BasicType>(get<Type::Set>(cdtype.variants).key));
            } else if constexpr (IsDynamicArray<T>) {
                bool is_vec = type_matches_tag<Type::Vector>(cdtype.variants);
                assert_true(type_matches_tag<Type::List>(cdtype.variants) || is_vec, "static value type requires ctype list/vector, this should never happen");

                if (!is_vec) {
                    w(reinterpret_cast<const U8*>(&v.length), sizeof(v.length));
                }
                const auto element_basic = is_vec ? get<BasicType>(get<Type::Vector>(cdtype.variants).element) : get<BasicType>(get<Type::List>(cdtype.variants).element);
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
    inline AutoString8 bytes_to_hex(const U8* data, U64 len) {
        const char hex_chars[] = "0123456789abcdef";
        AutoString8 result{len * 2};
        for (U64 i = 0; i < len; i++) {
            result.c_str[i*2]     = hex_chars[data[i] >> 4];
            result.c_str[i*2 + 1] = hex_chars[data[i] & 0xf];
        }
        return result;
    }

    AutoString8 to_str(const cql::ColumnValue& value, [[maybe_unused]] cql::BasicType dtype) {
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
            return "@todo"_as;
        });
    }

    AutoString8 to_str(const cql::ColumnValue& value, const cql::Type& cdtype) {
        if (type_matches_tag<cql::Type::Basic>(cdtype.variants))
            return to_str(value, get<cql::Type::Basic>(cdtype.variants).value_dtype);
        return visit(value, [&cdtype](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, DynamicArray<cql::NestedColumnValue>>) {
                bool is_vec = type_matches_tag<cql::Type::Vector>(cdtype.variants);
                const auto& elem = is_vec ? get<cql::Type::Vector>(cdtype.variants).element : get<cql::Type::List>(cdtype.variants).element;
                AutoString8 result = "["_as;
                bool first = true;
                for (const auto& el : v) {
                    if (!first) result = result + ", ";
                    result = result + to_str(el.value, get<cql::Type>(elem));
                    first = false;
                }
                return result + "]";
            } else if constexpr (SameAs<T, DynamicSet<cql::NestedColumnValue>>) {
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str((*it).value, get<cql::Type>(get<cql::Type::Set>(cdtype.variants).key));
                    first = false;
                }
                return result + "}";
            } else if constexpr (SameAs<T, DynamicMap<cql::NestedColumnValue, cql::NestedColumnValue>>) {
                const auto& m = get<cql::Type::Map>(cdtype.variants);
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str((*it).first.value, get<cql::Type>(m.key));
                    result = result + ": ";
                    result = result + to_str((*it).second.value, get<cql::Type>(m.value));
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicMap<T>) {
                const auto& m = get<cql::Type::Map>(cdtype.variants);
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str(cql::ColumnValue{(*it).first}, get<cql::BasicType>(m.key));
                    result = result + ": ";
                    result = result + to_str(cql::ColumnValue{(*it).second}, get<cql::BasicType>(m.value));
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicSet<T>) {
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str(cql::ColumnValue{*it}, get<cql::BasicType>(get<cql::Type::Set>(cdtype.variants).key));
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicArray<T>) {
                bool is_vec = type_matches_tag<cql::Type::Vector>(cdtype.variants);
                const auto& elem = is_vec ? get<cql::Type::Vector>(cdtype.variants).element : get<cql::Type::List>(cdtype.variants).element;
                AutoString8 result = "["_as;
                bool first = true;
                for (const auto& el : v) {
                    if (!first) result = result + ", ";
                    result = result + to_str(cql::ColumnValue{el}, get<cql::BasicType>(elem));
                    first = false;
                }
                return result + "]";
            } else {
                return to_str(cql::ColumnValue{v}, get<cql::Type::Basic>(cdtype.variants).value_dtype);
            }
        });
    }
}
