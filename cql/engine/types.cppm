export module cql.engine.types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

using namespace plexdb;

export namespace cql {
    enum class BasicType : U8 {
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
        hex,
    };

    struct Type;

    struct ElementType : HybridTaggedUnion<TypeList<BasicType>, TypeList<Type>> {
        using HybridTaggedUnion::HybridTaggedUnion;
        static ElementType from(Type t);  // defined after Type is complete
    };

    bool operator==(const Type& a, const Type& b);

    struct Type {
        struct Basic  { BasicType value_dtype;                                    };
        struct List   { ElementType element; bool frozen;                         };
        struct Set    { ElementType key;     bool frozen;                         };
        struct Map    { ElementType key; ElementType value; bool frozen;          };
        struct Vector { ElementType element; U64 count; bool frozen;              };

        TaggedUnion<Basic, List, Set, Map, Vector> variants;
    };

    inline bool operator==(const Type::Basic&  a, const Type::Basic&  b) { return a.value_dtype == b.value_dtype; }
    inline bool operator==(const Type::List&   a, const Type::List&   b) { return a.element == b.element && a.frozen == b.frozen; }
    inline bool operator==(const Type::Set&    a, const Type::Set&    b) { return a.key == b.key && a.frozen == b.frozen; }
    inline bool operator==(const Type::Map&    a, const Type::Map&    b) { return a.key == b.key && a.value == b.value && a.frozen == b.frozen; }
    inline bool operator==(const Type::Vector& a, const Type::Vector& b) { return a.element == b.element && a.count == b.count && a.frozen == b.frozen; }

    inline bool operator==(const Type& a, const Type& b) { return a.variants == b.variants; }

    inline ElementType ElementType::from(Type t) {
        if (type_matches_tag<Type::Basic>(t.variants))
            return ElementType{get<Type::Basic>(t.variants).value_dtype};
        return ElementType{move(t)};
    }

    inline Type create_basic(BasicType d) { return Type{.variants=Type::Basic{d}}; }
    inline Type create_list(BasicType el) { return Type{.variants=Type::List{ElementType{el}, false}}; }
    inline Type create_list(ElementType el) { return Type{.variants=Type::List{move(el), false}}; }
    inline Type create_set(BasicType key) { return Type{.variants=Type::Set{ElementType{key}, false}}; }
    inline Type create_set(ElementType key) { return Type{.variants=Type::Set{move(key), false}}; }
    inline Type create_map(BasicType key, BasicType val) { return Type{.variants=Type::Map{ElementType{key}, ElementType{val}, false}}; }
    inline Type create_map(ElementType key, ElementType val) { return Type{.variants=Type::Map{move(key), move(val), false}}; }
    inline Type create_vector(BasicType el, U64 count) { return Type{.variants=Type::Vector{ElementType{el}, count, false}}; }
    inline Type create_vector(ElementType el, U64 count) { return Type{.variants=Type::Vector{move(el), count, false}}; }
}

export namespace plexdb {
    U64 hash(const cql::Type& t) {
        U8 buf[18];
        U64 len = 0;
        buf[len++] = static_cast<U8>(t.variants.index);
        visit(t.variants, [&](const auto& v) {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, cql::Type::Basic>) {
                buf[len++] = static_cast<U8>(v.value_dtype);
            } else if constexpr (SameAs<T, cql::Type::List>) {
                if (type_matches_tag<cql::BasicType>(v.element))
                    buf[len++] = static_cast<U8>(get<cql::BasicType>(v.element));
            } else if constexpr (SameAs<T, cql::Type::Set>) {
                if (type_matches_tag<cql::BasicType>(v.key))
                    buf[len++] = static_cast<U8>(get<cql::BasicType>(v.key));
            } else if constexpr (SameAs<T, cql::Type::Map>) {
                if (type_matches_tag<cql::BasicType>(v.key))
                    buf[len++] = static_cast<U8>(get<cql::BasicType>(v.key));
                if (type_matches_tag<cql::BasicType>(v.value))
                    buf[len++] = static_cast<U8>(get<cql::BasicType>(v.value));
            } else if constexpr (SameAs<T, cql::Type::Vector>) {
                if (type_matches_tag<cql::BasicType>(v.element))
                    buf[len++] = static_cast<U8>(get<cql::BasicType>(v.element));
                os::memory_copy(buf + len, &v.count, sizeof(U64));
                len += sizeof(U64);
            }
        });
        return hash(String8(buf, len));
    }

    constexpr inline String8 to_str(cql::BasicType dtype) {
        switch (dtype) {
            case cql::BasicType::text:       return "text";
            case cql::BasicType::int_:       return "int";
            case cql::BasicType::bigint:     return "bigint";
            case cql::BasicType::smallint:   return "smallint";
            case cql::BasicType::counter:    return "counter";
            case cql::BasicType::timestamp:  return "timestamp";
            case cql::BasicType::boolean:    return "boolean";
            case cql::BasicType::float_:     return "float";
            case cql::BasicType::double_:    return "double";
            case cql::BasicType::uuid:       return "uuid";
            case cql::BasicType::ascii:      return "ascii";
            case cql::BasicType::blob:       return "blob";
            case cql::BasicType::date:       return "date";
            case cql::BasicType::decimal:    return "decimal";
            case cql::BasicType::duration:   return "duration";
            case cql::BasicType::inet:       return "inet";
            case cql::BasicType::time:       return "time";
            case cql::BasicType::timeuuid:   return "timeuuid";
            case cql::BasicType::tinyint:    return "tinyint";
            case cql::BasicType::varchar:    return "varchar";
            case cql::BasicType::varint:     return "varint";
            case cql::BasicType::hex:        return "hex";
        }
        return "unknown";
    }

    inline AutoString8 to_str(const cql::ElementType& e);

    inline AutoString8 to_str(cql::Type cdtype) {
        return visit(cdtype.variants, [](const auto& v) -> AutoString8 {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, cql::Type::Basic>)
                return AutoString8(to_str(v.value_dtype));
            else if constexpr (SameAs<T, cql::Type::List>)
                return "list["_as + to_str(v.element) + "]";
            else if constexpr (SameAs<T, cql::Type::Set>)
                return "set["_as + to_str(v.key) + "]";
            else if constexpr (SameAs<T, cql::Type::Map>)
                return "map["_as + to_str(v.key) + ", " + to_str(v.value) + "]";
            else {
                static_assert(SameAs<T, cql::Type::Vector>, "unhandled Type variant in to_str");
                return "vector["_as + to_str(v.element) + "]";
            }
        });
    }

    inline AutoString8 to_str(const cql::ElementType& e) {
        if (type_matches_tag<cql::BasicType>(e)) return AutoString8(to_str(get<cql::BasicType>(e)));
        return to_str(get<cql::Type>(e));
    }
}
