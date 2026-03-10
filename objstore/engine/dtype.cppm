export module objstore.engine.dtype;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

using namespace plexdb;

namespace {
    // dynamic array
    template<typename List, typename = void>
    struct ExpandDynamicArrayHelper;

    template<typename... Ts>
    struct ExpandDynamicArrayHelper<TypeList<Ts...>> {
        using type = TypeList<DynamicArray<Ts>...>;
    };

    template<typename Ts>
    using ExpandDynamicArray = ExpandDynamicArrayHelper<Ts>::type;

    // dynamic map
    template<typename K, typename ValueList>
    struct ExpandDynamicMapValuesHelper;

    template<typename K, typename... Vs>
    struct ExpandDynamicMapValuesHelper<K, TypeList<Vs...>> {
        using type = TypeList<DynamicMap<K, Vs>...>;
    };

    template<typename Ks, typename Vs>
    struct ExpandDynamicMapHelper;

    template<typename K1, typename... Ks, typename Vs>
    struct ExpandDynamicMapHelper<TypeList<K1, Ks...>, Vs> {
    private:
        using K1Maps = typename ExpandDynamicMapValuesHelper<K1, Vs>::type;
        using K2NMaps = typename ExpandDynamicMapHelper<TypeList<Ks...>, Vs>::type;
    public:
        using type = Concat<K1Maps, K2NMaps>;
    };

    template<typename Vs>
    struct ExpandDynamicMapHelper<TypeList<>, Vs> {
        using type = TypeList<>;
    };

    template<typename Ks, typename Vs>
    using ExpandDynamicMap = ExpandDynamicMapHelper<Ks, Vs>::type;

    // dynamic set
    template<typename List, typename = void>
    struct ExpandDynamicSetHelper;

    template<typename... Ts>
    struct ExpandDynamicSetHelper<TypeList<Ts...>> {
        using type = TypeList<DynamicSet<Ts>...>;
    };

    template<typename Ts>
    using ExpandDynamicSet = ExpandDynamicSetHelper<Ts>::type;

    // tagged union
    template<typename... Ts>
    struct ExpandTaggedUnionHelper;

    template<typename... Ts>
    struct ExpandTaggedUnionHelper<TypeList<Ts...>> {
        using type = TaggedUnion<Ts...>;
    };

    template<typename Ts>
    using ExpandTaggedUnion = ExpandTaggedUnionHelper<Ts>::type;

    // concepts
    template<typename T> struct IsPairHelper                        { static constexpr bool is = false; };
    template<typename U, typename V> struct IsPairHelper<Pair<U,V>> { static constexpr bool is = true;  };

    template<typename T>
    concept IsPair = IsPairHelper<T>::is;
}

export namespace objstore {
    enum class CType : U8 {
        native,
        list,
        map,
        set,
    };

    enum class DType : U8 {
        text,
        uuid,
        timestamp,
        smallint,
        int_,
        bigint,
        counter,
        boolean,
        float_,
        double_,
    };

    union CDType {
        struct base {
            CType ctype;
        } base;
        struct native {
            CType ctype;
            DType value_dtype;
        } native;
        struct list {
            CType ctype;
            DType element_dtype;
        } list;
        struct set {
            CType ctype;
            DType key_dtype;
        } set;
        struct map {
            CType ctype;
            DType key_dtype;
            DType value_dtype;
        } map;
    };

    constexpr bool operator==(CDType a, CDType b) {
        if (a.base.ctype != b.base.ctype) return false;
        switch (a.base.ctype) {
            case CType::native: return a.native.value_dtype == b.native.value_dtype;
            case CType::list:   return a.list.element_dtype == b.list.element_dtype;
            case CType::set:    return a.set.key_dtype == b.set.key_dtype;
            case CType::map:    return a.map.key_dtype == b.map.key_dtype && a.map.value_dtype == b.map.value_dtype;
        }
        return false;
    }

    namespace dtype {
        constexpr CDType make_native(DType d)               { return CDType{.native = {CType::native, d}}; }
        constexpr CDType make_list(DType el)                { return CDType{.list   = {CType::list,   el}}; }
        constexpr CDType make_set(DType key)                { return CDType{.set    = {CType::set,    key}}; }
        constexpr CDType make_map(DType key, DType val)     { return CDType{.map    = {CType::map,    key, val}}; }

        using WriteNativeTypes = TypeList<AutoString8, S64, bool, F64>;
        using ReadNativeTypes = TypeList<AutoString8, S64, S32, S16, U8, F64, F32>;

        using WriteNativeValue = ExpandTaggedUnion<WriteNativeTypes>;
        using WriteValue = ExpandTaggedUnion<Concat<
            WriteNativeTypes,
            TypeList<DynamicArray<WriteNativeValue>, DynamicArray<Pair<WriteNativeValue, WriteNativeValue>>>
        >>;

        using ReadValue = ExpandTaggedUnion<Concat<
            ReadNativeTypes,
            ExpandDynamicArray<ReadNativeTypes>,
            ExpandDynamicMap<ReadNativeTypes, ReadNativeTypes>,
            ExpandDynamicSet<ReadNativeTypes>
        >>;

        inline constexpr U64 hash(const WriteValue& value) {
            return visit(value, [](auto& v) -> U64 {
                using T = Decay<decltype(v)>;
    
                if constexpr (Either<T, S64, bool, F64>) {
                    return os::memory_cast<U64>(&v);
                } else {
                    return hash(v);
                }
            });
        }
        
        constexpr inline String8 to_str(DType dtype) {
            switch (dtype) {
                case DType::text:       return "text";
                case DType::int_:       return "int";
                case DType::bigint:     return "bigint";
                case DType::smallint:   return "smallint";
                case DType::counter:    return "counter";
                case DType::timestamp:  return "timestamp";
                case DType::boolean:    return "boolean";
                case DType::float_:     return "float";
                case DType::double_:    return "double";
                case DType::uuid:       return "uuid";
            }
            return "unknown";
        }
        inline AutoString8 to_str(CDType cdtype) {
            switch (cdtype.base.ctype) {
                case CType::native: return AutoString8(to_str(cdtype.native.value_dtype));
                case CType::list:   return "list["_as + to_str(cdtype.list.element_dtype) + "]";
                case CType::set:    return "set["_as + to_str(cdtype.set.key_dtype) + "]";
                case CType::map:    return "map["_as + to_str(cdtype.map.key_dtype) + ", " + to_str(cdtype.map.value_dtype) + "]";
            }
            return "unknown"_as;
        }
        
        template<typename T>
        bool can_write_generic(DType dtype) {
            if constexpr (Either<RemoveCV<T>, String8>) {
                return dtype == DType::text || dtype == DType::timestamp || dtype == DType::uuid;
            }
            if constexpr (Either<RemoveCV<T>, S64>) {
                return dtype == DType::int_ || dtype == DType::bigint || dtype == DType::smallint || dtype == DType::counter;
            }
            if constexpr (Either<RemoveCV<T>, bool>) {
                return dtype == DType::boolean;
            }
            if constexpr (Either<RemoveCV<T>, F64, F32>) {
                return dtype == DType::float_ || dtype == DType::double_;
            }
            
            return false;
        }

        bool can_write_generic(const WriteNativeValue& src, DType dtype) {
            return visit(src, [&dtype](auto& v) {
                using T = Decay<decltype(v)>;
                using V = Conditional<SameAs<T, AutoString8>, String8, T>;

                return can_write_generic<V>(dtype);
            });
        }

        bool can_write_generic(const WriteValue& src, CDType cdtype) {
            return visit(src, [&cdtype](auto& v) {
                using T = Decay<decltype(v)>;
                using V = Conditional<SameAs<T, AutoString8>, String8, T>;

                if constexpr (IsDynamicArray<T>) {
                    if constexpr (IsPair<typename T::Element>) {
                        static_assert(SameAs<typename T::Element, Pair<WriteNativeValue, WriteNativeValue>>);
                        for (const auto& pair_v : v) {
                            if (!can_write_generic(pair_v.first, cdtype.map.key_dtype)) return false;
                            if (!can_write_generic(pair_v.second, cdtype.map.value_dtype)) return false;
                        }
                        return true;
                    } else {
                        static_assert(SameAs<typename T::Element, WriteNativeValue>);

                        if (cdtype.base.ctype == CType::list) {
                            for (const auto& element_v : v) {
                                if (!can_write_generic(element_v, cdtype.list.element_dtype)) return false;
                            }
                            return true;
                        } else {
                            for (const auto& key_v : v) {
                                if (!can_write_generic(key_v, cdtype.set.key_dtype)) return false;
                            }
                            return true;
                        }
                    }
                }

                return can_write_generic<V>(cdtype.native.value_dtype);
            });
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
        void write_specific_default(const Write auto& w, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::timestamp:
                case DType::uuid:{
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }break;
                case DType::smallint:{
                    S16 smallint = 0_s16;
                    w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                }break;
                case DType::int_:{
                    S32 int_ = 0_s32;
                    w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                }break;
                case DType::counter:
                case DType::bigint:{
                    S64 bigint = 0_s64;
                    w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                }break;
                case DType::boolean:{
                    U8 boolean = false;
                    w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                }break;
                case DType::float_:{
                    F32 float_ = 0_f32;
                    w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                }break;
                case DType::double_:{
                    F64 double_ = 0_f64;
                    w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                }break;
            }
        }

        void write_specific_default(const Write auto& w, CDType cdtype) {
            switch (cdtype.base.ctype) {
                case CType::native:{
                    write_specific_default(w, cdtype.native.value_dtype);
                }break;
                case CType::list:
                case CType::set:
                case CType::map:{
                    U64 length = 0_u64;
                    w(reinterpret_cast<const U8*>(&length), sizeof(length));
                }break;
            }
        }
    
        template<typename T>
        void write_generic(const Write auto& w, const T& src, DType dtype) {
            if constexpr (SameAs<T, AutoString8>) {
                switch (dtype) {
                    case DType::text:
                    case DType::timestamp:
                    case DType::uuid:{
                        w(reinterpret_cast<const U8*>(&src.length), sizeof(src.length));
                        w(reinterpret_cast<const U8*>(src.c_str), src.length);
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else if constexpr (SameAs<T, S64>) {
                switch (dtype) {
                    case DType::smallint:{
                        S16 smallint = static_cast<S16>(src);
                        w(reinterpret_cast<const U8*>(&smallint), sizeof(smallint));
                    }break;
                    case DType::int_:{
                        S32 int_ = static_cast<S32>(src);
                        w(reinterpret_cast<const U8*>(&int_), sizeof(int_));
                    }break;
                    case DType::counter:
                    case DType::bigint:{
                        S64 bigint = static_cast<S64>(src);
                        w(reinterpret_cast<const U8*>(&bigint), sizeof(bigint));
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else if constexpr (SameAs<T, bool>) {
                switch (dtype) {
                    case DType::boolean:{
                        U8 boolean = static_cast<U8>(src);
                        w(reinterpret_cast<const U8*>(&boolean), sizeof(boolean));
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else if constexpr (SameAs<T, F64>) {
                switch (dtype) {
                    case DType::float_:{
                        F32 float_ = static_cast<F32>(src);
                        w(reinterpret_cast<const U8*>(&float_), sizeof(float_));
                    }break;
                    case DType::double_:{
                        F64 double_ = static_cast<F64>(src);
                        w(reinterpret_cast<const U8*>(&double_), sizeof(double_));
                    }break;
                    default:{ assert_true(false, "mismatch between underlying type and dtype"); }break;
                }
            } else {
                static_assert(!SameAs<T,T>, "missing underlying type case");
            }
        }

        void write_generic(const Write auto& w, const WriteNativeValue& src, DType dtype) {
            visit(src, [&](const auto& v) {
                write_generic(w, v, dtype);
            });
        }

        void write_generic(const Write auto& w, const WriteValue& src, CDType cdtype) {
            visit(src, [&cdtype, &w](const auto& v) {
                using T = Decay<decltype(v)>;

                if constexpr (IsDynamicArray<T>) {
                    w(reinterpret_cast<const U8*>(&v.length), sizeof(v.length));
                    if constexpr (IsPair<typename T::Element>) {
                        for (const auto& el: v) {
                            write_generic(w, el.first, cdtype.map.key_dtype);
                            write_generic(w, el.second, cdtype.map.value_dtype);
                        }
                    } else {
                        if (cdtype.base.ctype == CType::list) {
                            for (const auto& el: v) write_generic(w, el, cdtype.list.element_dtype);
                        } else {
                            for (const auto& el: v) write_generic(w, el, cdtype.set.key_dtype);
                        }
                    }
                } else {
                    write_generic(w, v, cdtype.native.value_dtype);
                }
            });
        }

        template<typename T>
        bool can_write_specific(DType dtype) {
            if constexpr (Either<RemoveCV<T>, AutoString8>) {
                return dtype == DType::text || dtype == DType::timestamp || dtype == DType::uuid;
            }
            if constexpr (Either<RemoveCV<T>, S16>) { return dtype == DType::smallint; }
            if constexpr (Either<RemoveCV<T>, S32>) { return dtype == DType::int_; }
            if constexpr (Either<RemoveCV<T>, S64>) { return dtype == DType::bigint || dtype == DType::counter; }
            if constexpr (Either<RemoveCV<T>, U8>)  { return dtype == DType::boolean; }
            if constexpr (Either<RemoveCV<T>, F32>) { return dtype == DType::float_; }
            if constexpr (Either<RemoveCV<T>, F64>) { return dtype == DType::double_; }
            return false;
        }

        bool can_write_specific(const ReadValue& src, CDType cdtype) {
            return visit(src, [&cdtype](const auto& v) -> bool {
                using T = Decay<decltype(v)>;
                if constexpr (IsDynamicMap<T>) {
                    if (cdtype.base.ctype != CType::map) return false;
                    return can_write_specific<typename T::Key>(cdtype.map.key_dtype) &&
                           can_write_specific<typename T::Value>(cdtype.map.value_dtype);
                } else if constexpr (IsDynamicSet<T>) {
                    if (cdtype.base.ctype != CType::set) return false;
                    return can_write_specific<typename T::Key>(cdtype.set.key_dtype);
                } else if constexpr (IsDynamicArray<T>) {
                    if (cdtype.base.ctype != CType::list) return false;
                    return can_write_specific<typename T::Element>(cdtype.list.element_dtype);
                } else {
                    return cdtype.base.ctype == CType::native && can_write_specific<T>(cdtype.native.value_dtype);
                }
            });
        }

        template<typename T>
        void write_specific(const Write auto& w, const T& src, DType dtype) {
            if constexpr (SameAs<T, AutoString8> || SameAs<T, String8>) {
                String8 s{src};
                w(reinterpret_cast<const U8*>(&s.length), sizeof(s.length));
                w(reinterpret_cast<const U8*>(s.data), s.length);
            } else {
                w(reinterpret_cast<const U8*>(&src), sizeof(src));
            }
        }

        void write_specific(const Write auto& w, const ReadValue& src, CDType cdtype) {
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
    
        ReadValue read(const Read auto& r, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::uuid:
                case DType::timestamp:{
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));

                    AutoString8 value{length};
                    r(reinterpret_cast<U8*>(value.c_str), length);
                
                    return {move(value)};
                }break;
                case DType::smallint:{
                    S16 value; 
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::int_:{
                    S32 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::counter:
                case DType::bigint:{
                    S64 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::boolean:{
                    U8 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::float_:{
                    F32 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
                case DType::double_:{
                    F64 value;
                    r(reinterpret_cast<U8*>(&value), sizeof(value));
    
                    return {move(value)};
                }break;
            }
        }

        AutoString8 to_str(const ReadValue& value, DType dtype) {
            switch (dtype) {
                case DType::text:
                case DType::uuid:
                case DType::timestamp:{
                    return get<AutoString8>(value);
                }break;
                case DType::smallint:{
                    return plexdb::to_str(get<S16>(value));
                }break;
                case DType::int_:{
                    return plexdb::to_str(get<S32>(value));
                }break;
                case DType::counter:
                case DType::bigint:{
                    return plexdb::to_str(get<S64>(value));
                }break;
                case DType::boolean:{
                    return static_cast<bool>(get<U8>(value)) ? "true"_as : "false"_as;
                }break;
                case DType::float_:{
                    return plexdb::to_str(get<F32>(value));
                }break;
                case DType::double_:{
                    return plexdb::to_str(get<F64>(value));
                }break;
            }
        }

        ReadValue read_specific(const Read auto& r, CDType cdtype) {
            switch (cdtype.base.ctype) {
                case CType::native:
                    return read(r, cdtype.native.value_dtype);
                case CType::list: {
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));
                    auto read_arr = [&]<typename El>() -> ReadValue {
                        DynamicArray<El> arr{};
                        for (U64 i = 0; i < length; i++)
                            push_back(arr, get<El>(read(r, cdtype.list.element_dtype)));
                        return {move(arr)};
                    };
                    switch (cdtype.list.element_dtype) {
                        case DType::text: case DType::uuid: case DType::timestamp:
                            return read_arr.template operator()<AutoString8>();
                        case DType::smallint:  return read_arr.template operator()<S16>();
                        case DType::int_:      return read_arr.template operator()<S32>();
                        case DType::bigint: case DType::counter:
                                               return read_arr.template operator()<S64>();
                        case DType::boolean:   return read_arr.template operator()<U8>();
                        case DType::float_:    return read_arr.template operator()<F32>();
                        case DType::double_:   return read_arr.template operator()<F64>();
                    }
                } break;
                case CType::set: {
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));
                    auto read_set = [&]<typename K>() -> ReadValue {
                        DynamicSet<K> s{};
                        for (U64 i = 0; i < length; i++)
                            insert(s, get<K>(read(r, cdtype.set.key_dtype)));
                        return {move(s)};
                    };
                    switch (cdtype.set.key_dtype) {
                        case DType::text: case DType::uuid: case DType::timestamp:
                            return read_set.template operator()<AutoString8>();
                        case DType::smallint:  return read_set.template operator()<S16>();
                        case DType::int_:      return read_set.template operator()<S32>();
                        case DType::bigint: case DType::counter:
                                               return read_set.template operator()<S64>();
                        case DType::boolean:   return read_set.template operator()<U8>();
                        case DType::float_:    return read_set.template operator()<F32>();
                        case DType::double_:   return read_set.template operator()<F64>();
                    }
                } break;
                case CType::map: {
                    U64 length;
                    r(reinterpret_cast<U8*>(&length), sizeof(length));
                    auto read_map_kv = [&]<typename K, typename V>() -> ReadValue {
                        DynamicMap<K, V> m{};
                        for (U64 i = 0; i < length; i++) {
                            K k = get<K>(read(r, cdtype.map.key_dtype));
                            V v = get<V>(read(r, cdtype.map.value_dtype));
                            insert(m, move(k), move(v));
                        }
                        return {move(m)};
                    };
                    auto read_map_v = [&]<typename K>(DType val_dtype) -> ReadValue {
                        switch (val_dtype) {
                            case DType::text: case DType::uuid: case DType::timestamp:
                                return read_map_kv.template operator()<K, AutoString8>();
                            case DType::smallint:  return read_map_kv.template operator()<K, S16>();
                            case DType::int_:      return read_map_kv.template operator()<K, S32>();
                            case DType::bigint: case DType::counter:
                                                   return read_map_kv.template operator()<K, S64>();
                            case DType::boolean:   return read_map_kv.template operator()<K, U8>();
                            case DType::float_:    return read_map_kv.template operator()<K, F32>();
                            case DType::double_:   return read_map_kv.template operator()<K, F64>();
                        }
                    };
                    switch (cdtype.map.key_dtype) {
                        case DType::text: case DType::uuid: case DType::timestamp:
                            return read_map_v.template operator()<AutoString8>(cdtype.map.value_dtype);
                        case DType::smallint:  return read_map_v.template operator()<S16>(cdtype.map.value_dtype);
                        case DType::int_:      return read_map_v.template operator()<S32>(cdtype.map.value_dtype);
                        case DType::bigint: case DType::counter:
                                               return read_map_v.template operator()<S64>(cdtype.map.value_dtype);
                        case DType::boolean:   return read_map_v.template operator()<U8>(cdtype.map.value_dtype);
                        case DType::float_:    return read_map_v.template operator()<F32>(cdtype.map.value_dtype);
                        case DType::double_:   return read_map_v.template operator()<F64>(cdtype.map.value_dtype);
                    }
                } break;
            }
        }

        AutoString8 to_str(const ReadValue& value, CDType cdtype) {
            if (cdtype.base.ctype == CType::native)
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
