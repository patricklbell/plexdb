export module objstore.engine.types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;

using namespace plexdb;

export namespace objstore {
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
        
        struct Basic  { BasicType value_dtype;                       };
        struct List   { BasicType element_dtype;                     };
        struct Set    { BasicType key_dtype;                         };
        struct Map    { BasicType key_dtype; BasicType value_dtype; };
        struct Vector { BasicType element_dtype; U64 count;          };

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
            case CollectionType::basic: return a.basic.value_dtype == b.basic.value_dtype;
            case CollectionType::list:   return a.list.element_dtype == b.list.element_dtype;
            case CollectionType::set:    return a.set.key_dtype == b.set.key_dtype;
            case CollectionType::map:    return a.map.key_dtype == b.map.key_dtype && a.map.value_dtype == b.map.value_dtype;
            case CollectionType::vector: return a.vector.element_dtype == b.vector.element_dtype && a.vector.count == b.vector.count;
        }
        return false;
    }


    constexpr Type make_basic(BasicType d) {
        return Type{.ctype = CollectionType::basic, .basic = {.value_dtype = d}};
    }

    constexpr Type make_list(BasicType el) {
        return Type{.ctype = CollectionType::list, .list = {.element_dtype = el}};
    }

    constexpr Type make_set(BasicType key) {
        return Type{.ctype = CollectionType::set, .set = {.key_dtype = key}};
    }

    constexpr Type make_map(BasicType key, BasicType val) {
        return Type{.ctype = CollectionType::map, .map = {.key_dtype = key, .value_dtype = val}};
    }

    constexpr Type make_vector(BasicType el, U64 count) {
        return Type{.ctype = CollectionType::vector, .vector = {.element_dtype = el, .count = count}};
    }
}

export namespace plexdb {
    constexpr inline String8 to_str(objstore::BasicType dtype) {
        switch (dtype) {
            case objstore::BasicType::text:       return "text";
            case objstore::BasicType::int_:       return "int";
            case objstore::BasicType::bigint:     return "bigint";
            case objstore::BasicType::smallint:   return "smallint";
            case objstore::BasicType::counter:    return "counter";
            case objstore::BasicType::timestamp:  return "timestamp";
            case objstore::BasicType::boolean:    return "boolean";
            case objstore::BasicType::float_:     return "float";
            case objstore::BasicType::double_:    return "double";
            case objstore::BasicType::uuid:       return "uuid";
            case objstore::BasicType::ascii:      return "ascii";
            case objstore::BasicType::blob:       return "blob";
            case objstore::BasicType::date:       return "date";
            case objstore::BasicType::decimal:    return "decimal";
            case objstore::BasicType::duration:   return "duration";
            case objstore::BasicType::inet:       return "inet";
            case objstore::BasicType::time:       return "time";
            case objstore::BasicType::timeuuid:   return "timeuuid";
            case objstore::BasicType::tinyint:    return "tinyint";
            case objstore::BasicType::varchar:    return "varchar";
            case objstore::BasicType::varint:     return "varint";
            case objstore::BasicType::vector:     return "vector";
            case objstore::BasicType::hex:        return "hex";
        }
        return "unknown";
    }
    constexpr inline AutoString8 to_str(objstore::Type cdtype) {
        switch (cdtype.ctype) {
            case objstore::CollectionType::basic:  return AutoString8(to_str(cdtype.basic.value_dtype));
            case objstore::CollectionType::list:   return "list["_as + to_str(cdtype.list.element_dtype) + "]";
            case objstore::CollectionType::set:    return "set["_as + to_str(cdtype.set.key_dtype) + "]";
            case objstore::CollectionType::map:    return "map["_as + to_str(cdtype.map.key_dtype) + ", " + to_str(cdtype.map.value_dtype) + "]";
            case objstore::CollectionType::vector: return "vector["_as + to_str(cdtype.vector.element_dtype) + "]";
        }
        return "unknown"_as;
    }
}
