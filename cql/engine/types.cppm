export module cql.engine.types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

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
        vector,
        hex,
    };

    enum class CollectionType : U8 {
        basic,
        list,
        map,
        set,
        vector,
    };

    struct Type {
        CollectionType ctype;

        struct Basic  { BasicType value_dtype;                                    };
        struct List   { BasicType element_dtype; bool frozen;                     };
        struct Set    { BasicType key_dtype; bool frozen;                         };
        struct Map    { BasicType key_dtype; BasicType value_dtype; bool frozen;  };
        struct Vector { BasicType element_dtype; U64 count; bool frozen;          };

        union {
            struct Basic basic;
            struct List list;
            struct Set set;
            struct Map map;
            struct Vector vector;
        };
    };

    constexpr bool operator==(Type a, Type b) {
        if (a.ctype != b.ctype) return false;
        switch (a.ctype) {
            case CollectionType::basic:  return a.basic.value_dtype == b.basic.value_dtype;
            case CollectionType::list:   return a.list.element_dtype == b.list.element_dtype  && a.list.frozen == b.list.frozen;
            case CollectionType::set:    return a.set.key_dtype == b.set.key_dtype && a.set.frozen == b.set.frozen;
            case CollectionType::map:    return a.map.key_dtype == b.map.key_dtype && a.map.value_dtype == b.map.value_dtype && a.map.frozen == b.map.frozen;
            case CollectionType::vector: return a.vector.element_dtype == b.vector.element_dtype && a.vector.count == b.vector.count && a.vector.frozen == b.vector.frozen;
        }
        return false;
    }


    constexpr Type create_basic(BasicType d) {
        return Type{.ctype = CollectionType::basic, .basic = {.value_dtype = d}};
    }

    constexpr Type create_list(BasicType el) {
        return Type{.ctype = CollectionType::list, .list = {.element_dtype = el, .frozen = false}};
    }

    constexpr Type create_set(BasicType key) {
        return Type{.ctype = CollectionType::set, .set = {.key_dtype = key, .frozen = false}};
    }

    constexpr Type create_map(BasicType key, BasicType val) {
        return Type{.ctype = CollectionType::map, .map = {.key_dtype = key, .value_dtype = val, .frozen = false}};
    }

    constexpr Type create_vector(BasicType el, U64 count) {
        return Type{.ctype = CollectionType::vector, .vector = {.element_dtype = el, .count = count, .frozen = false}};
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
                buf[len++] = static_cast<U8>(t.list.element_dtype);
                break;
            case cql::CollectionType::set:
                buf[len++] = static_cast<U8>(t.set.key_dtype);
                break;
            case cql::CollectionType::map:
                buf[len++] = static_cast<U8>(t.map.key_dtype);
                buf[len++] = static_cast<U8>(t.map.value_dtype);
                break;
            case cql::CollectionType::vector:
                buf[len++] = static_cast<U8>(t.vector.element_dtype);
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
            case cql::BasicType::vector:     return "vector";
            case cql::BasicType::hex:        return "hex";
        }
        return "unknown";
    }
    constexpr inline AutoString8 to_str(cql::Type cdtype) {
        switch (cdtype.ctype) {
            case cql::CollectionType::basic:  return AutoString8(to_str(cdtype.basic.value_dtype));
            case cql::CollectionType::list:   return "list["_as + to_str(cdtype.list.element_dtype) + "]";
            case cql::CollectionType::set:    return "set["_as + to_str(cdtype.set.key_dtype) + "]";
            case cql::CollectionType::map:    return "map["_as + to_str(cdtype.map.key_dtype) + ", " + to_str(cdtype.map.value_dtype) + "]";
            case cql::CollectionType::vector: return "vector["_as + to_str(cdtype.vector.element_dtype) + "]";
        }
        return "unknown"_as;
    }
}
