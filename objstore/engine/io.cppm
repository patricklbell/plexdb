export module objstore.engine.io;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import objstore.engine.evaluator;
import objstore.engine.types;
import objstore.engine.statements;

using namespace plexdb;

export namespace objstore {
    // @todo support UDTs
    using ColumnValueBasicTypes = TypeList<AutoString8, S64, S32, S16, U8, F64, F32, Blob, UUID>;
    using ColumnValueTypes = Concat<
        ColumnValueBasicTypes,
        ExpandDynamicArray<ColumnValueBasicTypes>,
        ExpandDynamicMap<ColumnValueBasicTypes, ColumnValueBasicTypes>,
        ExpandDynamicSet<ColumnValueBasicTypes>,
        TypeList<Null>
    >;
    using ColumnValue = ExpandTaggedUnion<ColumnValueTypes>;
}

export namespace objstore::io {
    template<typename F>
    concept Read = requires(F f, U8* src, U64 size) {
        f(src, size);
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
    ColumnValue read_column_value(const Read auto& r, BasicType dtype) {
        switch (dtype) {
            case BasicType::text:{
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));

                AutoString8 value{length};
                r(reinterpret_cast<U8*>(value.c_str), length);

                return {move(value)};
            }break;
            case BasicType::smallint:{
                S16 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::int_:{
                S32 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::counter:
            case BasicType::timestamp:
            case BasicType::bigint:{
                S64 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::boolean:{
                U8 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::float_:{
                F32 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::double_:{
                F64 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::uuid:{
                UUID value;
                r(&value.value[0], value.length);

                return {move(value)};
            }break;
            case BasicType::blob:{
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));

                Blob value;
                resize(value.value, length);
                assert_true(value.value.length == length, "resize successful");
                r(value.value.ptr, length);

                return {move(value)};
            }
            case BasicType::ascii:
            case BasicType::varchar:{
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));

                AutoString8 value{length};
                r(reinterpret_cast<U8*>(value.c_str), length);

                return {move(value)};
            }break;
            case BasicType::tinyint:{
                U8 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::date:{
                S32 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::time:{
                S64 value;
                r(reinterpret_cast<U8*>(&value), sizeof(value));

                return {move(value)};
            }break;
            case BasicType::timeuuid:{
                UUID value;
                r(&value.value[0], value.length);

                return {move(value)};
            }break;
            case BasicType::inet:
            case BasicType::varint:
            case BasicType::decimal:
            case BasicType::duration:
            case BasicType::vector:
            case BasicType::hex:{
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));

                Blob value;
                resize(value.value, length);
                assert_true(value.value.length == length, "resize successful");
                r(value.value.ptr, length);

                return {move(value)};
            }
        }

        assert_true(false, "invalid basic type for read");
        return {};
    }

    ColumnValue read_column_value(const Read auto& r, const Type& cdtype) {
        switch (cdtype.ctype) {
            case CollectionType::basic:
                return read_column_value(r, cdtype.basic.value_dtype);
            case CollectionType::list: {
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));
                auto read_arr = [&]<typename El>() -> ColumnValue {
                    DynamicArray<El> arr{};
                    for (U64 i = 0; i < length; i++)
                        push_back(arr, get<El>(read_column_value(r, cdtype.list.element_dtype)));
                    return {move(arr)};
                };
                switch (cdtype.list.element_dtype) {
                    case BasicType::text:       return read_arr.template operator()<AutoString8>();
                    case BasicType::smallint:   return read_arr.template operator()<S16>();
                    case BasicType::int_:       return read_arr.template operator()<S32>();
                    case BasicType::bigint:
                    case BasicType::timestamp:
                    case BasicType::counter:
                    case BasicType::time:       return read_arr.template operator()<S64>();
                    case BasicType::boolean:    return read_arr.template operator()<U8>();
                    case BasicType::float_:     return read_arr.template operator()<F32>();
                    case BasicType::double_:    return read_arr.template operator()<F64>();
                    case BasicType::uuid:
                    case BasicType::timeuuid:   return read_arr.template operator()<UUID>();
                    case BasicType::tinyint:    return read_arr.template operator()<U8>();
                    case BasicType::date:       return read_arr.template operator()<S32>();
                    case BasicType::ascii:
                    case BasicType::varchar:    return read_arr.template operator()<AutoString8>();
                    case BasicType::blob:
                    case BasicType::inet:
                    case BasicType::varint:
                    case BasicType::decimal:
                    case BasicType::duration:
                    case BasicType::vector:
                    case BasicType::hex:        return read_arr.template operator()<Blob>();
                }

                assert_true(false, "invalid basic type for list read");
                return {};
            } break;
            case CollectionType::set: {
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));
                auto read_set = [&]<typename K>() -> ColumnValue {
                    DynamicSet<K> s{};
                    for (U64 i = 0; i < length; i++)
                        insert(s, get<K>(read_column_value(r, cdtype.set.key_dtype)));
                    return {move(s)};
                };
                switch (cdtype.set.key_dtype) {
                    case BasicType::text:       return read_set.template operator()<AutoString8>();
                    case BasicType::smallint:   return read_set.template operator()<S16>();
                    case BasicType::int_:       return read_set.template operator()<S32>();
                    case BasicType::bigint:
                    case BasicType::timestamp:
                    case BasicType::counter:
                    case BasicType::time:       return read_set.template operator()<S64>();
                    case BasicType::boolean:    return read_set.template operator()<U8>();
                    case BasicType::float_:     return read_set.template operator()<F32>();
                    case BasicType::double_:    return read_set.template operator()<F64>();
                    case BasicType::uuid:
                    case BasicType::timeuuid:   return read_set.template operator()<UUID>();
                    case BasicType::tinyint:    return read_set.template operator()<U8>();
                    case BasicType::date:       return read_set.template operator()<S32>();
                    case BasicType::ascii:
                    case BasicType::varchar:    return read_set.template operator()<AutoString8>();
                    case BasicType::blob:
                    case BasicType::inet:
                    case BasicType::varint:
                    case BasicType::decimal:
                    case BasicType::duration:
                    case BasicType::vector:
                    case BasicType::hex:        return read_set.template operator()<Blob>();
                }

                assert_true(false, "invalid basic type for set read");
                return {};
            } break;
            case CollectionType::map: {
                U64 length;
                r(reinterpret_cast<U8*>(&length), sizeof(length));
                auto read_map_kv = [&]<typename K, typename V>() -> ColumnValue {
                    DynamicMap<K, V> m{};
                    for (U64 i = 0; i < length; i++) {
                        K k = get<K>(read_column_value(r, cdtype.map.key_dtype));
                        V v = get<V>(read_column_value(r, cdtype.map.value_dtype));
                        insert(m, move(k), move(v));
                    }
                    return {move(m)};
                };
                auto read_map_v = [&]<typename K>(BasicType val_dtype) -> ColumnValue {
                    switch (val_dtype) {
                        case BasicType::text:       return read_map_kv.template operator()<K, AutoString8>();
                        case BasicType::smallint:   return read_map_kv.template operator()<K, S16>();
                        case BasicType::int_:       return read_map_kv.template operator()<K, S32>();
                        case BasicType::bigint:
                        case BasicType::timestamp:
                        case BasicType::counter:
                        case BasicType::time:       return read_map_kv.template operator()<K, S64>();
                        case BasicType::boolean:    return read_map_kv.template operator()<K, U8>();
                        case BasicType::float_:     return read_map_kv.template operator()<K, F32>();
                        case BasicType::double_:    return read_map_kv.template operator()<K, F64>();
                        case BasicType::uuid:
                        case BasicType::timeuuid:   return read_map_kv.template operator()<K, UUID>();
                        case BasicType::tinyint:    return read_map_kv.template operator()<K, U8>();
                        case BasicType::date:       return read_map_kv.template operator()<K, S32>();
                        case BasicType::ascii:
                        case BasicType::varchar:    return read_map_kv.template operator()<K, AutoString8>();
                        case BasicType::blob:
                        case BasicType::inet:
                        case BasicType::varint:
                        case BasicType::decimal:
                        case BasicType::duration:
                        case BasicType::vector:
                        case BasicType::hex:        return read_map_kv.template operator()<K, Blob>();
                    }
                    assert_true(false, "invalid basic type for map value read");
                    return {};
                };
                switch (cdtype.map.key_dtype) {
                    case BasicType::text:       return read_map_v.template operator()<AutoString8>(cdtype.map.value_dtype);
                    case BasicType::smallint:   return read_map_v.template operator()<S16>(cdtype.map.value_dtype);
                    case BasicType::int_:       return read_map_v.template operator()<S32>(cdtype.map.value_dtype);
                    case BasicType::bigint:
                    case BasicType::timestamp:
                    case BasicType::counter:
                    case BasicType::time:       return read_map_v.template operator()<S64>(cdtype.map.value_dtype);
                    case BasicType::boolean:    return read_map_v.template operator()<U8>(cdtype.map.value_dtype);
                    case BasicType::float_:     return read_map_v.template operator()<F32>(cdtype.map.value_dtype);
                    case BasicType::double_:    return read_map_v.template operator()<F64>(cdtype.map.value_dtype);
                    case BasicType::uuid:
                    case BasicType::timeuuid:   return read_map_v.template operator()<UUID>(cdtype.map.value_dtype);
                    case BasicType::tinyint:    return read_map_v.template operator()<U8>(cdtype.map.value_dtype);
                    case BasicType::date:       return read_map_v.template operator()<S32>(cdtype.map.value_dtype);
                    case BasicType::ascii:
                    case BasicType::varchar:    return read_map_v.template operator()<AutoString8>(cdtype.map.value_dtype);
                    case BasicType::blob:
                    case BasicType::inet:
                    case BasicType::varint:
                    case BasicType::decimal:
                    case BasicType::duration:
                    case BasicType::vector:
                    case BasicType::hex:        return read_map_v.template operator()<Blob>(cdtype.map.value_dtype);
                }
                assert_true(false, "invalid basic type for map key read");
                return {};
            } break;
            case CollectionType::vector:{
                assert_not_implemented("reading vector collection type is not implemented");
            }break;
        }
        assert_true(false, "invalid collection type");
        return {};
    }

    void read_column_mask(const Read auto& r, const MarkColumnActive auto& mark_active) {
        U64 column_count;
        r(reinterpret_cast<U8*>(&column_count), sizeof(column_count));
        static_assert(sizeof(column_count) == COLUMN_COUNT_BYTE_COUNT);

        for (U64 mask_idx = 0; mask_idx < ceil_div(column_count, MASK_BIT_COUNT); mask_idx++) {
            U64 mask;
            r(reinterpret_cast<U8*>(&mask), sizeof(mask));

            for (U64 bit_idx = 0; bit_idx < min(MASK_BIT_COUNT, column_count - mask_idx*MASK_BIT_COUNT); bit_idx++) {
                if (mask & (1_u64 << bit_idx)) {
                    mark_active(mask_idx*MASK_BIT_COUNT + bit_idx);
                }
            }
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
                   dtype == BasicType::duration || dtype == BasicType::vector || dtype == BasicType::hex;
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
                   dtype == BasicType::decimal || dtype == BasicType::duration || dtype == BasicType::vector ||
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
        if constexpr (Either<RemoveCV<T>, AutoString8>) { return dtype == BasicType::text || dtype == BasicType::ascii || dtype == BasicType::varchar; }
        else if constexpr (Either<RemoveCV<T>, S16>)    { return dtype == BasicType::smallint; }
        else if constexpr (Either<RemoveCV<T>, S32>)    { return dtype == BasicType::int_ || dtype == BasicType::date; }
        else if constexpr (Either<RemoveCV<T>, S64>)    { return dtype == BasicType::bigint || dtype == BasicType::timestamp || dtype == BasicType::counter || dtype == BasicType::time; }
        else if constexpr (Either<RemoveCV<T>, U8>)     { return dtype == BasicType::boolean || dtype == BasicType::tinyint; }
        else if constexpr (Either<RemoveCV<T>, F32>)    { return dtype == BasicType::float_; }
        else if constexpr (Either<RemoveCV<T>, F64>)    { return dtype == BasicType::double_; }
        else if constexpr (Either<RemoveCV<T>, UUID>)   { return dtype == BasicType::uuid || dtype == BasicType::timeuuid; }
        else if constexpr (Either<RemoveCV<T>, Null>)   { return true;}
        else if constexpr (Either<RemoveCV<T>, Blob>)   { return dtype == BasicType::blob || dtype == BasicType::inet || dtype == BasicType::varint || dtype == BasicType::decimal || dtype == BasicType::duration || dtype == BasicType::vector || dtype == BasicType::hex; }
        else                                            { static_assert(!SameAs<T,T>, "missing type case"); return false; }
    }

    bool can_cast_write_constant_as_column_value(const Constant& constant, BasicType dtype) {
        return visit(constant.value, [&](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            return can_cast_write_typed_basic_as_column_value<T>(dtype);
        });
    }

    bool can_cast_write_evaluated_as_column_value(const Evaluated& evaluated, const Type& cdtype) {
        return visit(evaluated.value, [&](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                return cdtype.ctype == CollectionType::basic && can_cast_write_constant_as_column_value(cv, cdtype.basic.value_dtype);
            } else if constexpr (SameAs<T, MapLiteral>) {
                assert_not_implemented("writing map literal as column value is not implemented");
                return false;
            } else if constexpr (SameAs<T, SetLiteral>) {
                assert_not_implemented("writing set literal as column value is not implemented");
                return false;
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                assert_not_implemented("writing list/vector literal as column value is not implemented");
                return false;
            } else if constexpr (SameAs<T, UdtLiteral>) {
                assert_not_implemented("writing UDT literal as column value is not implemented");
                return false;
            } else if constexpr (SameAs<T, TupleLiteral>) {
                assert_not_implemented("writing tuple literal as column value is not implemented");
                return false;
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
            case BasicType::inet:
            case BasicType::varint:
            case BasicType::decimal:
            case BasicType::duration:
            case BasicType::vector:
            case BasicType::hex:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
        }
    }

    void write_default_column_value(const Write auto& w, const Type& cdtype) {
        switch (cdtype.ctype) {
            case CollectionType::basic:{
                write_default_column_value(w, cdtype.basic.value_dtype);
            }break;
            case CollectionType::list:
            case CollectionType::set:
            case CollectionType::map:
            case CollectionType::vector:{
                U64 length = 0_u64;
                w(reinterpret_cast<const U8*>(&length), sizeof(length));
            }break;
        }
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
                case BasicType::vector:
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
                        dtype == BasicType::decimal || dtype == BasicType::duration || dtype == BasicType::vector ||
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

    void cast_write_evaluated_as_column_value(const Write auto& w, const Evaluated& evaluated, const Type& cdtype) {
        assert_true(can_cast_write_evaluated_as_column_value(evaluated, cdtype), "invalid evaluated value for write");

        visit(evaluated.value, [&](const auto& cv) {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                assert_true(cdtype.ctype == CollectionType::basic, "bad can write check!");
                cast_write_constant_as_column_value(w, cv, cdtype.basic.value_dtype);
            } else if constexpr (SameAs<T, MapLiteral>) {
                assert_not_implemented("writing map literal as column value is not implemented");
            } else if constexpr (SameAs<T, SetLiteral>) {
                assert_not_implemented("writing set literal as column value is not implemented");
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                assert_not_implemented("writing list/vector literal as column value is not implemented");
            } else if constexpr (SameAs<T, UdtLiteral>) {
                assert_not_implemented("writing UDT literal as column value is not implemented");
            } else if constexpr (SameAs<T, TupleLiteral>) {
                assert_not_implemented("writing tuple literal as column value is not implemented");
            } else {
                static_assert(!SameAs<T,T>, "missing type case");
            }
        });
    }

    bool can_write_column_value(const ColumnValue& value, const Type& cdtype) {
        return visit(value, [&cdtype](const auto& v) -> bool {
            using T = Decay<decltype(v)>;

            // @warn can_write_typed_basic_as_column_value is permissive whereas ColumnValue should really be exact,
            //       this introduces an implicit cast which may be undesirable.
            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                return cdtype.ctype == CollectionType::basic && can_write_typed_basic_as_column_value<T>(cdtype.basic.value_dtype);
            } else if constexpr (IsDynamicMap<T>) {
                if (cdtype.ctype != CollectionType::map) return false;
                return can_write_typed_basic_as_column_value<typename T::Key>(cdtype.map.key_dtype) &&
                       can_write_typed_basic_as_column_value<typename T::Value>(cdtype.map.value_dtype);
            } else if constexpr (IsDynamicSet<T>) {
                if (cdtype.ctype != CollectionType::set) return false;
                return can_write_typed_basic_as_column_value<typename T::Key>(cdtype.set.key_dtype);
            } else if constexpr (IsDynamicArray<T>) {
                if (cdtype.ctype != CollectionType::list) return false;
                return can_write_typed_basic_as_column_value<typename T::Element>(cdtype.list.element_dtype);
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
        } else {
            static_assert(!SameAs<T,T>, "missing type case");
        }
    }

    void write_column_value(const Write auto& w, const ColumnValue& value, const Type& cdtype) {
        assert_true(can_write_column_value(value, cdtype), "invalid type to write for cdtype " + to_str(cdtype));
        visit(value, [&cdtype, &w](auto& v) {
            using T = Decay<decltype(v)>;

            if constexpr (IsInTypeList<T, ColumnValueBasicTypes>) {
                assert_true(cdtype.ctype == CollectionType::basic, "static value type requires ctype basic, this should never happen");

                write_typed_basic(w, v, cdtype.basic.value_dtype);
            } else if constexpr (IsDynamicMap<T>) {
                assert_true(cdtype.ctype == CollectionType::map, "static value type requires ctype map, this should never happen");

                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (auto it = v.begin(); it != v.end(); ++it) {
                    write_typed_basic(w, (*it).first, cdtype.map.key_dtype);
                    write_typed_basic(w, (*it).second, cdtype.map.value_dtype);
                }
            } else if constexpr (IsDynamicSet<T>) {
                assert_true(cdtype.ctype == CollectionType::set, "static value type requires ctype set, this should never happen");

                U64 len = length(v);
                w(reinterpret_cast<const U8*>(&len), sizeof(len));
                for (auto it = v.begin(); it != v.end(); ++it)
                    write_typed_basic(w, *it, cdtype.set.key_dtype);
            } else if constexpr (IsDynamicArray<T>) {
                assert_true(cdtype.ctype == CollectionType::list || cdtype.ctype == CollectionType::vector, "static value type requires ctype list/vector, this should never happen");

                w(reinterpret_cast<const U8*>(&v.length), sizeof(v.length));
                for (const auto& el: v)
                    write_typed_basic(w, el, cdtype.list.element_dtype);
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
    AutoString8 to_str(const objstore::ColumnValue& value, [[maybe_unused]] objstore::BasicType dtype) {
        return visit(value, [](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (Either<T, AutoString8, S64, S32, S16, U8, F64, F32>) {
                return to_str(v);
            }
            return "@todo"_as;
        });
    }

    AutoString8 to_str(const objstore::ColumnValue& value, const objstore::Type& cdtype) {
        if (cdtype.ctype == objstore::CollectionType::basic)
            return to_str(value, cdtype.basic.value_dtype);
        return visit(value, [&cdtype](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (IsDynamicMap<T>) {
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str(objstore::ColumnValue{(*it).first}, cdtype.map.key_dtype);
                    result = result + ": ";
                    result = result + to_str(objstore::ColumnValue{(*it).second}, cdtype.map.value_dtype);
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicSet<T>) {
                AutoString8 result = "{"_as;
                bool first = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) result = result + ", ";
                    result = result + to_str(objstore::ColumnValue{*it}, cdtype.set.key_dtype);
                    first = false;
                }
                return result + "}";
            } else if constexpr (IsDynamicArray<T>) {
                AutoString8 result = "["_as;
                bool first = true;
                for (const auto& el : v) {
                    if (!first) result = result + ", ";
                    result = result + to_str(objstore::ColumnValue{el}, cdtype.list.element_dtype);
                    first = false;
                }
                return result + "]";
            } else {
                return to_str(objstore::ColumnValue{v}, cdtype.basic.value_dtype);
            }
        });
    }
}
