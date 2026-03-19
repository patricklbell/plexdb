export module objstore.engine.dtype;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

using namespace plexdb;

export namespace objstore {
    enum class NativeType : U8 {
        ascii,
        bigint,
        blob,
        boolean,
        counter,
        date,
        decimal,
        double_,
        duration,
        float_,
        inet,
        int_,
        smallint,
        text,
        time,
        timestamp,
        timeuuid,
        tinyint,
        uuid,
        varchar,
        varint,
        vector,
    };

    enum class ClassType : U8 {
        native,
        list,
        map,
        set,
        vector,
    };

    union CqlType {
        struct base {
            ClassType ctype;
        } base;
        struct native {
            ClassType ctype;
            NativeType value_dtype;
        } native;
        struct list {
            ClassType ctype;
            NativeType element_dtype;
        } list;
        struct set {
            ClassType ctype;
            NativeType key_dtype;
        } set;
        struct map {
            ClassType ctype;
            NativeType key_dtype;
            NativeType value_dtype;
        } map;
        struct vector {
            ClassType ctype;
            NativeType element_dtype;
            U64 count;
        } vector_;
    };

    constexpr bool operator==(CqlType a, CqlType b) {
        if (a.base.ctype != b.base.ctype) return false;
        switch (a.base.ctype) {
            case ClassType::native: return a.native.value_dtype == b.native.value_dtype;
            case ClassType::list:   return a.list.element_dtype == b.list.element_dtype;
            case ClassType::set:    return a.set.key_dtype == b.set.key_dtype;
            case ClassType::map:    return a.map.key_dtype == b.map.key_dtype && a.map.value_dtype == b.map.value_dtype;
            case ClassType::vector: return a.vector_.element_dtype == b.vector_.element_dtype && a.vector_.count == b.vector_.count;
        }
        return false;
    }

    namespace types {
        using enum NativeType;

        constexpr CqlType make_native(NativeType d)               { return CqlType{.native = {ClassType::native, d}}; }
        constexpr CqlType make_list(NativeType el)                { return CqlType{.list   = {ClassType::list,   el}}; }
        constexpr CqlType make_set(NativeType key)                { return CqlType{.set    = {ClassType::set,    key}}; }
        constexpr CqlType make_map(NativeType key, NativeType val)     { return CqlType{.map    = {ClassType::map,    key, val}}; }

        // @todo read UDT
        using ReadNativeTypes = TypeList<AutoString8, S64, S32, S16, U8, F64, F32>;
        using ReadTypes = Concat<
            ReadNativeTypes,
            ExpandDynamicArray<ReadNativeTypes>,
            ExpandDynamicMap<ReadNativeTypes, ReadNativeTypes>,
            ExpandDynamicSet<ReadNativeTypes>
        >;
        using ReadValue = ExpandTaggedUnion<ReadTypes>;
        
        constexpr inline String8 to_str(NativeType dtype) {
            switch (dtype) {
                case types::text:       return "text";
                case types::int_:       return "int";
                case types::bigint:     return "bigint";
                case types::smallint:   return "smallint";
                case types::counter:    return "counter";
                case types::timestamp:  return "timestamp";
                case types::boolean:    return "boolean";
                case types::float_:     return "float";
                case types::double_:    return "double";
                case types::uuid:       return "uuid";
                case types::ascii:      return "ascii";
                case types::blob:       return "blob";
                case types::date:       return "date";
                case types::decimal:    return "decimal";
                case types::duration:   return "duration";
                case types::inet:       return "inet";
                case types::time:       return "time";
                case types::timeuuid:   return "timeuuid";
                case types::tinyint:    return "tinyint";
                case types::varchar:    return "varchar";
                case types::varint:     return "varint";
                case types::vector:     return "vector";
            }
            return "unknown";
        }
        inline AutoString8 to_str(CqlType cdtype) {
            switch (cdtype.base.ctype) {
                case ClassType::native: return AutoString8(to_str(cdtype.native.value_dtype));
                case ClassType::list:   return "list["_as + to_str(cdtype.list.element_dtype) + "]";
                case ClassType::set:    return "set["_as + to_str(cdtype.set.key_dtype) + "]";
                case ClassType::map:    return "map["_as + to_str(cdtype.map.key_dtype) + ", " + to_str(cdtype.map.value_dtype) + "]";
                case ClassType::vector: return "vector["_as + to_str(cdtype.vector_.element_dtype) + "]";
            }
            return "unknown"_as;
        }
        
        template<typename T>
        bool can_write_generic(NativeType dtype) {
            if constexpr (Either<RemoveCV<T>, String8, AutoString8>) {
                return dtype == types::text || dtype == types::timestamp || dtype == types::uuid;
            }
            if constexpr (Either<RemoveCV<T>, S64>) {
                return dtype == types::int_ || dtype == types::bigint || dtype == types::smallint || dtype == types::counter;
            }
            if constexpr (Either<RemoveCV<T>, bool>) {
                return dtype == types::boolean;
            }
            if constexpr (Either<RemoveCV<T>, F64, F32>) {
                return dtype == types::float_ || dtype == types::double_;
            }
            
            return false;
        }

        template<typename F>
        concept Write = requires(F f, const U8* src, U64 size) {
            f(src, size);
        };
    
        template<typename F>
        concept Read = requires(F f, U8* src, U64 size) {
            f(src, size);
        };
    
        // @todo avoid copy for zero init
        void write_specific_default(const Write auto& w, NativeType dtype) {
            switch (dtype) {
                case types::text:
                case types::timestamp:
                case types::uuid:{
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }break;
                case types::smallint:{
                    S16 smallint = 0_s16;
                    w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                }break;
                case types::int_:{
                    S32 int_ = 0_s32;
                    w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                }break;
                case types::counter:
                case types::bigint:{
                    S64 bigint = 0_s64;
                    w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                }break;
                case types::boolean:{
                    U8 boolean = false;
                    w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                }break;
                case types::float_:{
                    F32 float_ = 0_f32;
                    w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                }break;
                case types::double_:{
                    F64 double_ = 0_f64;
                    w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                }break;
                case types::ascii:
                case types::blob:
                case types::date:
                case types::decimal:
                case types::duration:
                case types::inet:
                case types::time:
                case types::timeuuid:
                case types::tinyint:
                case types::varchar:
                case types::varint:
                case types::vector:
                    break;
            }
        }

        void write_specific_default(const Write auto& w, CqlType cdtype) {
            switch (cdtype.base.ctype) {
                case ClassType::native:{
                    write_specific_default(w, cdtype.native.value_dtype);
                }break;
                case ClassType::list:
                case ClassType::set:
                case ClassType::map:
                case ClassType::vector:{
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }break;
            }
        }
    
        template<typename T>
        void write_generic(const Write auto& w, const T& src, NativeType dtype) {
            if constexpr (SameAs<T, AutoString8>) {
                switch (dtype) {
                    case types::text:
                    case types::timestamp:
                    case types::uuid:{
                        w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                        w(reinterpret_cast<const U8*>(src.c_str), src.length);
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else if constexpr (SameAs<T, S64>) {
                switch (dtype) {
                    case types::smallint:{
                        S16 smallint = static_cast<S16>(src);
                        w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                    }break;
                    case types::int_:{
                        S32 int_ = static_cast<S32>(src);
                        w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                    }break;
                    case types::counter:
                    case types::bigint:{
                        S64 bigint = static_cast<S64>(src);
                        w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else if constexpr (SameAs<T, bool>) {
                switch (dtype) {
                    case types::boolean:{
                        U8 boolean = static_cast<U8>(src);
                        w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else if constexpr (SameAs<T, F64>) {
                switch (dtype) {
                    case types::float_:{
                        F32 float_ = static_cast<F32>(src);
                        w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                    }break;
                    case types::double_:{
                        F64 double_ = static_cast<F64>(src);
                        w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else {
                static_assert(!SameAs<T,T>, "missing underlying type case");
            }
        }

        // @note these accept any TaggedUnion containing primitive types (e.g. Constant.value)
        template<typename TU>
        bool can_write_constant_value(const TU& value, NativeType dtype) {
            return visit(value, [&](const auto& cv) -> bool {
                using CT = Decay<decltype(cv)>;
                if constexpr (Either<CT, S64, AutoString8, F64, bool>) {
                    return can_write_generic<CT>(dtype);
                }
                return false;
            });
        }

        template<typename TU>
        void write_constant_value(const Write auto& w, const TU& value, NativeType dtype) {
            visit(value, [&](const auto& cv) {
                using CT = Decay<decltype(cv)>;
                if constexpr (Either<CT, S64, AutoString8, F64, bool>) {
                    write_generic(w, cv, dtype);
                }
            });
        }

        template<typename TU>
        U64 hash_constant_value(const TU& value) {
            return visit(value, [](const auto& cv) -> U64 {
                using CT = Decay<decltype(cv)>;
                if constexpr (SameAs<CT, S64>) return hash(static_cast<U64>(cv));
                if constexpr (SameAs<CT, AutoString8>) return hash(cv);
                return 0_u64;
            });
        }

        template<typename T>
        bool can_write_specific(NativeType dtype) {
            if constexpr (Either<RemoveCV<T>, AutoString8>) {
                return dtype == types::text || dtype == types::timestamp || dtype == types::uuid;
            }
            if constexpr (Either<RemoveCV<T>, S16>) { return dtype == types::smallint; }
            if constexpr (Either<RemoveCV<T>, S32>) { return dtype == types::int_; }
            if constexpr (Either<RemoveCV<T>, S64>) { return dtype == types::bigint || dtype == types::counter; }
            if constexpr (Either<RemoveCV<T>, U8>)  { return dtype == types::boolean; }
            if constexpr (Either<RemoveCV<T>, F32>) { return dtype == types::float_; }
            if constexpr (Either<RemoveCV<T>, F64>) { return dtype == types::double_; }
            return false;
        }

        bool can_write_specific(const ReadValue& src, CqlType cdtype) {
            return visit(src, [&cdtype](const auto& v) -> bool {
                using T = Decay<decltype(v)>;
                if constexpr (IsDynamicMap<T>) {
                    if (cdtype.base.ctype != ClassType::map) return false;
                    return can_write_specific<typename T::Key>(cdtype.map.key_dtype) &&
                           can_write_specific<typename T::Value>(cdtype.map.value_dtype);
                } else if constexpr (IsDynamicSet<T>) {
                    if (cdtype.base.ctype != ClassType::set) return false;
                    return can_write_specific<typename T::Key>(cdtype.set.key_dtype);
                } else if constexpr (IsDynamicArray<T>) {
                    if (cdtype.base.ctype != ClassType::list) return false;
                    return can_write_specific<typename T::Element>(cdtype.list.element_dtype);
                } else {
                    return cdtype.base.ctype == ClassType::native && can_write_specific<T>(cdtype.native.value_dtype);
                }
            });
        }

        template<typename T>
        void write_specific(const Write auto& w, const T& src, NativeType dtype) {
            if constexpr (SameAs<T, AutoString8> || SameAs<T, String8>) {
                String8 s{src};
                w(reinterpret_cast<const U8*>(&s.length), sizeof(s.length));
                w(reinterpret_cast<const U8*>(s.data), s.length);
            } else {
                w(reinterpret_cast<const U8*>(&src), sizeof(src));
            }
        }

        void write_specific(const Write auto& w, const ReadValue& src, CqlType cdtype) {
            assert_true(can_write_specific(src, cdtype), "invalid type to write for cdtype " + to_str(cdtype));
            visit(src, [&cdtype, &w](auto& v) {
                using T = Decay<decltype(v)>;

                if constexpr (IsDynamicMap<T>) {
                    U64 len = length(v);
                    w(reinterpret_cast<const U8*>(&len), sizeof(len));
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        write_specific(w, (*it).first, cdtype.map.key_dtype);
                        write_specific(w, (*it).second, cdtype.map.value_dtype);
                    }
                } else if constexpr (IsDynamicSet<T>) {
                    U64 len = length(v);
                    w(reinterpret_cast<const U8*>(&len), sizeof(len));
                    for (auto it = v.begin(); it != v.end(); ++it)
                        write_specific(w, *it, cdtype.set.key_dtype);
                } else if constexpr (IsDynamicArray<T>) {
                    w(reinterpret_cast<const U8*>(&v.length), sizeof(v.length));
                    for (const auto& el: v) write_specific(w, el, cdtype.list.element_dtype);
                } else {
                    write_specific(w, v, cdtype.native.value_dtype);
                }
            });
        }

        template<typename F>
        concept IsColumnActive = requires (F f, U64 idx) {
            { f(idx) } -> SameAs<bool>;
        };

        template<typename F>
        concept MarkColumnActive = requires (F f, U64 idx) {
            { f(idx) };
        };

        constexpr U64 U64_BIT_COUNT = sizeof(U64)*8_u64;

        void write_col_mask(const Write auto& w, const IsColumnActive auto& is_active, U64 column_count) {
            w(reinterpret_cast<const U8*>(&column_count), sizeof(column_count));
            
            U64 mask = 0;
            for (U64 idx = 0; idx < column_count; idx++) {
                mask |= (static_cast<U64>(is_active(idx)) << (idx % U64_BIT_COUNT));
                if (idx % U64_BIT_COUNT == U64_BIT_COUNT - 1) {
                    w(reinterpret_cast<const U8*>(&mask), sizeof(mask));
                    mask = 0;
                }
            }
            if (column_count % U64_BIT_COUNT != 0) {
                w(reinterpret_cast<const U8*>(&mask), sizeof(mask));
            }
        }

        void read_col_mask(const Read auto& r, const MarkColumnActive auto& mark_active) {
            U64 column_count;
            r(reinterpret_cast<U8*>(&column_count), sizeof(column_count));

            for (U64 mask_idx = 0; mask_idx < ceil_div(column_count, U64_BIT_COUNT); mask_idx++) {
                U64 mask;
                r(reinterpret_cast<U8*>(&mask), sizeof(mask));

                for (U64 bit_idx = 0; bit_idx < min(U64_BIT_COUNT, column_count - mask_idx*U64_BIT_COUNT); bit_idx++) {
                    if (mask & (1_u64 << bit_idx)) {
                        mark_active(mask_idx*U64_BIT_COUNT + bit_idx);
                    }
                }
            }
        }
    
        ReadValue read_specific(const Read auto& r, NativeType dtype) {
            switch (dtype) {
                case types::text:
                case types::uuid:
                case types::timestamp:{
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));

                    AutoString8 value{length};
                    r(reinterpret_cast<U8*>(value.c_str), length);
                
                    return {move(value)};
                }break;
                case types::smallint:{
                    S16 value; 
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case types::int_:{
                    S32 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case types::counter:
                case types::bigint:{
                    S64 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case types::boolean:{
                    U8 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case types::float_:{
                    F32 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case types::double_:{
                    F64 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case types::ascii:
                case types::blob:
                case types::date:
                case types::decimal:
                case types::duration:
                case types::inet:
                case types::time:
                case types::timeuuid:
                case types::tinyint:
                case types::varchar:
                case types::varint:
                case types::vector:
                    break;
            }
            assert_true(false, "unsupported native type for read");
            return {};
        }

        ReadValue read_specific(const Read auto& r, CqlType cdtype) {
            switch (cdtype.base.ctype) {
                case ClassType::native:
                    return read_specific(r, cdtype.native.value_dtype);
                case ClassType::list: {
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));
                    auto read_arr = [&]<typename El>() -> ReadValue {
                        DynamicArray<El> arr{};
                        for (U64 i = 0; i < length; i++)
                            push_back(arr, get<El>(read_specific(r, cdtype.list.element_dtype)));
                        return {move(arr)};
                    };
                    switch (cdtype.list.element_dtype) {
                        case types::text: case types::uuid: case types::timestamp:
                            return read_arr.template operator()<AutoString8>();
                        case types::smallint:  return read_arr.template operator()<S16>();
                        case types::int_:      return read_arr.template operator()<S32>();
                        case types::bigint: case types::counter:
                                               return read_arr.template operator()<S64>();
                        case types::boolean:   return read_arr.template operator()<U8>();
                        case types::float_:    return read_arr.template operator()<F32>();
                        case types::double_:   return read_arr.template operator()<F64>();
                        case types::ascii:
                        case types::blob:
                        case types::date:
                        case types::decimal:
                        case types::duration:
                        case types::inet:
                        case types::time:
                        case types::timeuuid:
                        case types::tinyint:
                        case types::varchar:
                        case types::varint:
                        case types::vector:
                            break;
                    }
                    assert_true(false, "unsupported native type for list read");
                    return {};
                } break;
                case ClassType::set: {
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));
                    auto read_set = [&]<typename K>() -> ReadValue {
                        DynamicSet<K> s{};
                        for (U64 i = 0; i < length; i++)
                            insert(s, get<K>(read_specific(r, cdtype.set.key_dtype)));
                        return {move(s)};
                    };
                    switch (cdtype.set.key_dtype) {
                        case types::text: case types::uuid: case types::timestamp:
                            return read_set.template operator()<AutoString8>();
                        case types::smallint:  return read_set.template operator()<S16>();
                        case types::int_:      return read_set.template operator()<S32>();
                        case types::bigint: case types::counter:
                                               return read_set.template operator()<S64>();
                        case types::boolean:   return read_set.template operator()<U8>();
                        case types::float_:    return read_set.template operator()<F32>();
                        case types::double_:   return read_set.template operator()<F64>();
                        case types::ascii:
                        case types::blob:
                        case types::date:
                        case types::decimal:
                        case types::duration:
                        case types::inet:
                        case types::time:
                        case types::timeuuid:
                        case types::tinyint:
                        case types::varchar:
                        case types::varint:
                        case types::vector:
                            break;
                    }
                    assert_true(false, "unsupported native type for set read");
                    return {};
                } break;
                case ClassType::map: {
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));
                    auto read_map_kv = [&]<typename K, typename V>() -> ReadValue {
                        DynamicMap<K, V> m{};
                        for (U64 i = 0; i < length; i++) {
                            K k = get<K>(read_specific(r, cdtype.map.key_dtype));
                            V v = get<V>(read_specific(r, cdtype.map.value_dtype));
                            insert(m, move(k), move(v));
                        }
                        return {move(m)};
                    };
                    auto read_map_v = [&]<typename K>(NativeType val_dtype) -> ReadValue {
                        switch (val_dtype) {
                            case types::text:
                            case types::uuid:
                            case types::timestamp:  return read_map_kv.template operator()<K, AutoString8>();
                            case types::smallint:   return read_map_kv.template operator()<K, S16>();
                            case types::int_:       return read_map_kv.template operator()<K, S32>();
                            case types::bigint:
                            case types::counter:    return read_map_kv.template operator()<K, S64>();
                            case types::boolean:    return read_map_kv.template operator()<K, U8>();
                            case types::float_:     return read_map_kv.template operator()<K, F32>();
                            case types::double_:    return read_map_kv.template operator()<K, F64>();
                            case types::ascii:
                            case types::blob:
                            case types::date:
                            case types::decimal:
                            case types::duration:
                            case types::inet:
                            case types::time:
                            case types::timeuuid:
                            case types::tinyint:
                            case types::varchar:
                            case types::varint:
                            case types::vector:     assert_true(false, "unsupported native type for read_map_v"); return {};
                        }
                    };
                    switch (cdtype.map.key_dtype) {
                        case types::text:
                        case types::uuid:
                        case types::timestamp:  return read_map_v.template operator()<AutoString8>(cdtype.map.value_dtype);
                        case types::smallint:   return read_map_v.template operator()<S16>(cdtype.map.value_dtype);
                        case types::int_:       return read_map_v.template operator()<S32>(cdtype.map.value_dtype);
                        case types::bigint:
                        case types::counter:    return read_map_v.template operator()<S64>(cdtype.map.value_dtype);
                        case types::boolean:    return read_map_v.template operator()<U8>(cdtype.map.value_dtype);
                        case types::float_:     return read_map_v.template operator()<F32>(cdtype.map.value_dtype);
                        case types::double_:    return read_map_v.template operator()<F64>(cdtype.map.value_dtype);
                        case types::ascii:
                        case types::blob:
                        case types::date:
                        case types::decimal:
                        case types::duration:
                        case types::inet:
                        case types::time:
                        case types::timeuuid:
                        case types::tinyint:
                        case types::varchar:
                        case types::varint:
                        case types::vector:     assert_true(false, "unsupported native type for map key read"); return {};
                    }
                } break;
                case ClassType::vector:
                    break;
            }
            assert_true(false, "unsupported class type for read_specific");
            return {};
        }

        AutoString8 to_str(ReadValue value, NativeType dtype) {
            switch (dtype) {
                case types::text:
                case types::uuid:
                case types::timestamp:{
                    return get<AutoString8>(value);
                }break;
                case types::smallint:{
                    return plexdb::to_str(get<S16>(value));
                }break;
                case types::int_:{
                    return plexdb::to_str(get<S32>(value));
                }break;
                case types::counter:
                case types::bigint:{
                    return plexdb::to_str(get<S64>(value));
                }break;
                case types::boolean:{
                    return static_cast<bool>(get<U8>(value)) ? "true"_as : "false"_as;
                }break;
                case types::float_:{
                    return plexdb::to_str(get<F32>(value));
                }break;
                case types::double_:{
                    return plexdb::to_str(get<F64>(value));
                }break;
                case types::ascii:
                case types::blob:
                case types::date:
                case types::decimal:
                case types::duration:
                case types::inet:
                case types::time:
                case types::timeuuid:
                case types::tinyint:
                case types::varchar:
                case types::varint:
                case types::vector:
                    break;
            }
            return "unknown"_as;
        }

        AutoString8 to_str(ReadValue value, CqlType cdtype) {
            if (cdtype.base.ctype == ClassType::native)
                return to_str(value, cdtype.native.value_dtype);
            return visit(value, [&cdtype](auto& v) -> AutoString8 {
                using T = Decay<decltype(v)>;
                if constexpr (IsDynamicMap<T>) {
                    AutoString8 result = "{"_as;
                    bool first = true;
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        if (!first) result = result + ", ";
                        result = result + to_str(ReadValue{(*it).first}, cdtype.map.key_dtype);
                        result = result + ": ";
                        result = result + to_str(ReadValue{(*it).second}, cdtype.map.value_dtype);
                        first = false;
                    }
                    return result + "}";
                } else if constexpr (IsDynamicSet<T>) {
                    AutoString8 result = "{"_as;
                    bool first = true;
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        if (!first) result = result + ", ";
                        result = result + to_str(ReadValue{*it}, cdtype.set.key_dtype);
                        first = false;
                    }
                    return result + "}";
                } else if constexpr (IsDynamicArray<T>) {
                    AutoString8 result = "["_as;
                    bool first = true;
                    for (const auto& el : v) {
                        if (!first) result = result + ", ";
                        result = result + to_str(ReadValue{el}, cdtype.list.element_dtype);
                        first = false;
                    }
                    return result + "]";
                } else {
                    return to_str(ReadValue{v}, cdtype.native.value_dtype);
                }
            });
        }
    }
}
