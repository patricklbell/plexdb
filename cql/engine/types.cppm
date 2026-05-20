export module cql.engine.types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
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

    enum class CollectionType : U8 {
        basic,
        list,
        map,
        set,
        vector,
    };

    struct Type;  // forward declare for recursive ElementType

    struct ElementType {
        bool is_collection;
        union {
            BasicType basic;
            Type* nested;  // heap-allocated; valid when is_collection == true
        };

        static constexpr ElementType from_basic(BasicType b) {
            return ElementType{.is_collection = false, .basic = b};
        }
        static ElementType from_nested(Type* t) {
            ElementType e;
            e.is_collection = true;
            e.nested = t;
            return e;
        }
        static ElementType from(Type t);  // defined after Type is complete
    };

    struct Type {
        CollectionType ctype;

        struct Basic  { BasicType value_dtype;                                           };
        struct List   { ElementType element;                    bool frozen;             };
        struct Set    { ElementType key;                        bool frozen;             };
        struct Map    { ElementType key; ElementType value;     bool frozen;             };
        struct Vector { ElementType element; U64 count;         bool frozen;             };

        union {
            struct Basic basic;
            struct List list;
            struct Set set;
            struct Map map;
            struct Vector vector;
        };
    };

    constexpr bool operator==(Type a, Type b);  // forward-declare for element_type_eq

    inline bool element_type_eq(ElementType a, ElementType b) {
        if (a.is_collection != b.is_collection) return false;
        if (!a.is_collection) return a.basic == b.basic;
        // both are collection — compare recursively
        if (a.nested == nullptr || b.nested == nullptr) return a.nested == b.nested;
        return *a.nested == *b.nested;
    }

    constexpr bool operator==(Type a, Type b) {
        if (a.ctype != b.ctype) return false;
        switch (a.ctype) {
            case CollectionType::basic:  return a.basic.value_dtype == b.basic.value_dtype;
            case CollectionType::list:   return element_type_eq(a.list.element, b.list.element) && a.list.frozen == b.list.frozen;
            case CollectionType::set:    return element_type_eq(a.set.key, b.set.key) && a.set.frozen == b.set.frozen;
            case CollectionType::map:    return element_type_eq(a.map.key, b.map.key) && element_type_eq(a.map.value, b.map.value) && a.map.frozen == b.map.frozen;
            case CollectionType::vector: return element_type_eq(a.vector.element, b.vector.element) && a.vector.count == b.vector.count && a.vector.frozen == b.vector.frozen;
        }
        return false;
    }

    inline ElementType ElementType::from(Type t) {
        if (t.ctype == CollectionType::basic) return ElementType::from_basic(t.basic.value_dtype);
        Type* p = reinterpret_cast<Type*>(os::allocate(sizeof(Type)));
        new (p) Type(t);
        return ElementType::from_nested(p);
    }

    constexpr Type create_basic(BasicType d) {
        return Type{.ctype = CollectionType::basic, .basic = {.value_dtype = d}};
    }

    constexpr Type create_list(BasicType el) {
        Type t;
        t.ctype = CollectionType::list;
        t.list.element = ElementType::from_basic(el);
        t.list.frozen = false;
        return t;
    }

    inline Type create_list(ElementType el) {
        Type t;
        t.ctype = CollectionType::list;
        t.list.element = el;
        t.list.frozen = false;
        return t;
    }

    constexpr Type create_set(BasicType key) {
        Type t;
        t.ctype = CollectionType::set;
        t.set.key = ElementType::from_basic(key);
        t.set.frozen = false;
        return t;
    }

    inline Type create_set(ElementType key) {
        Type t;
        t.ctype = CollectionType::set;
        t.set.key = key;
        t.set.frozen = false;
        return t;
    }

    constexpr Type create_map(BasicType key, BasicType val) {
        Type t;
        t.ctype = CollectionType::map;
        t.map.key = ElementType::from_basic(key);
        t.map.value = ElementType::from_basic(val);
        t.map.frozen = false;
        return t;
    }

    inline Type create_map(ElementType key, ElementType val) {
        Type t;
        t.ctype = CollectionType::map;
        t.map.key = key;
        t.map.value = val;
        t.map.frozen = false;
        return t;
    }

    constexpr Type create_vector(BasicType el, U64 count) {
        Type t;
        t.ctype = CollectionType::vector;
        t.vector.element = ElementType::from_basic(el);
        t.vector.count = count;
        t.vector.frozen = false;
        return t;
    }

    inline Type create_vector(ElementType el, U64 count) {
        Type t;
        t.ctype = CollectionType::vector;
        t.vector.element = el;
        t.vector.count = count;
        t.vector.frozen = false;
        return t;
    }
}

export namespace plexdb {
    // @todo ensure hash ordering matches cassandra
    U64 hash(const cql::Type& t) {
        U8 buf[18];
        U64 len = 0;
        buf[len++] = static_cast<U8>(t.ctype);
        switch (t.ctype) {
            case cql::CollectionType::basic:
                buf[len++] = static_cast<U8>(t.basic.value_dtype);
                break;
            case cql::CollectionType::list:
                if (!t.list.element.is_collection)
                    buf[len++] = static_cast<U8>(t.list.element.basic);
                break;
            case cql::CollectionType::set:
                if (!t.set.key.is_collection)
                    buf[len++] = static_cast<U8>(t.set.key.basic);
                break;
            case cql::CollectionType::map:
                if (!t.map.key.is_collection)
                    buf[len++] = static_cast<U8>(t.map.key.basic);
                if (!t.map.value.is_collection)
                    buf[len++] = static_cast<U8>(t.map.value.basic);
                break;
            case cql::CollectionType::vector:
                if (!t.vector.element.is_collection)
                    buf[len++] = static_cast<U8>(t.vector.element.basic);
                os::memory_copy(buf + len, &t.vector.count, sizeof(U64));
                len += sizeof(U64);
                break;
        }
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

    inline AutoString8 to_str_element_type(const cql::ElementType& e);

    constexpr inline AutoString8 to_str(cql::Type cdtype) {
        switch (cdtype.ctype) {
            case cql::CollectionType::basic:  return AutoString8(to_str(cdtype.basic.value_dtype));
            case cql::CollectionType::list:   return "list["_as + to_str_element_type(cdtype.list.element) + "]";
            case cql::CollectionType::set:    return "set["_as + to_str_element_type(cdtype.set.key) + "]";
            case cql::CollectionType::map:    return "map["_as + to_str_element_type(cdtype.map.key) + ", " + to_str_element_type(cdtype.map.value) + "]";
            case cql::CollectionType::vector: return "vector["_as + to_str_element_type(cdtype.vector.element) + "]";
        }
        return "unknown"_as;
    }

    inline AutoString8 to_str_element_type(const cql::ElementType& e) {
        if (!e.is_collection) return AutoString8(to_str(e.basic));
        if (e.nested == nullptr) return "null"_as;
        return to_str(*e.nested);
    }
}
