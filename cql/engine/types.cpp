module cql.engine.types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

using namespace plexdb;

namespace {
    static U64 mix(U64 h, U64 x) {
        h ^= x;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
        return h;
    }
}

namespace plexdb {
    U64 hash(const cql::type::Basic& t) {
        return hash(static_cast<U8>(t));
    }

    U64 hash(const cql::type::List& t) {
        return mix(hash(t.element), static_cast<U64>(t.frozen));
    }

    U64 hash(const cql::type::Set& t) {
        return mix(hash(t.key), static_cast<U64>(t.frozen));
    }

    U64 hash(const cql::type::Map& t) {
        return mix(mix(hash(t.key), hash(t.value)), static_cast<U64>(t.frozen));
    }

    U64 hash(const cql::type::Vector& t) {
        return mix(mix(hash(t.element), static_cast<U64>(t.frozen)), static_cast<U64>(t.count));
    }

    U64 hash(const cql::type::Tuple& t) {
        U64 h = static_cast<U64>(t.frozen);
        for (U64 i = 0; i < t.elements.length; i++) {
            h = mix(h, hash(t.elements[i]));
        }
        return h;
    }

    U64 hash(const cql::type::UDT& t) {
        return mix(plexdb::hash(t.keyspace), plexdb::hash(t.name));
    }

    U64 hash(cql::type::UDT* t) {
        return t == nullptr ? 0_u64 : hash(*t);
    }

    U64 hash(const cql::type::Type& t) {
        return visit(t.value, [&](const auto& v) {
            return hash(v);
        });
    }

    String8 to_str(cql::type::Basic dtype) {
        switch (dtype) {
            case cql::type::Basic::text:
                return "text";
            case cql::type::Basic::int_:
                return "int";
            case cql::type::Basic::bigint:
                return "bigint";
            case cql::type::Basic::smallint:
                return "smallint";
            case cql::type::Basic::counter:
                return "counter";
            case cql::type::Basic::timestamp:
                return "timestamp";
            case cql::type::Basic::boolean:
                return "boolean";
            case cql::type::Basic::float_:
                return "float";
            case cql::type::Basic::double_:
                return "double";
            case cql::type::Basic::uuid:
                return "uuid";
            case cql::type::Basic::ascii:
                return "ascii";
            case cql::type::Basic::blob:
                return "blob";
            case cql::type::Basic::date:
                return "date";
            case cql::type::Basic::decimal:
                return "decimal";
            case cql::type::Basic::duration:
                return "duration";
            case cql::type::Basic::inet:
                return "inet";
            case cql::type::Basic::time:
                return "time";
            case cql::type::Basic::timeuuid:
                return "timeuuid";
            case cql::type::Basic::tinyint:
                return "tinyint";
            case cql::type::Basic::varchar:
                return "varchar";
            case cql::type::Basic::varint:
                return "varint";
            case cql::type::Basic::hex:
                return "hex";
            case cql::type::Basic::COUNT:
                break;
        }
        return "unknown";
    }

    AutoString8 to_str(cql::type::Type cdtype) {
        return visit(cdtype.value, [](const auto& v) -> AutoString8 {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, cql::type::Basic>) {
                return AutoString8(to_str(v));
            } else if constexpr (SameAs<T, cql::type::List>) {
                return "list<"_as + to_str(v.element) + ">";
            } else if constexpr (SameAs<T, cql::type::Set>) {
                return "set<"_as + to_str(v.key) + ">";
            } else if constexpr (SameAs<T, cql::type::Map>) {
                return "map<"_as + to_str(v.key) + ", " + to_str(v.value) + ">";
            } else if constexpr (SameAs<T, cql::type::Vector>) {
                return "vector["_as + to_str(v.element) + "]";
            } else if constexpr (SameAs<T, cql::type::Tuple>) {
                AutoString8 result = "tuple<"_as;
                for (U64 i = 0; i < v.elements.length; i++) {
                    if (i > 0) {
                        result += ", ";
                    }
                    result += String8(to_str(v.elements[i]));
                }
                result += ">";
                return result;
            } else {
                static_assert(SameAs<T, cql::type::UDT*>, "unhandled Type variant in to_str");
                assert_true(v != nullptr, "resolved UDT pointer is null");
                AutoString8 result;
                if (v->keyspace.length > 0) {
                    result = AutoString8(v->keyspace) + ".";
                }
                result += v->name;
                return result;
            }
        });
    }
}

namespace cql {
    bool type_compatible_for_assignment(type::Basic from, type::Basic to) {
        if (from == to) {
            return true;
        }
        if ((to == type::Basic::text || to == type::Basic::varchar) && from == type::Basic::ascii) {
            return true;
        }
        if ((to == type::Basic::text && from == type::Basic::varchar) || (to == type::Basic::varchar && from == type::Basic::text)) {
            return true;
        }
        return false;
    }
}
