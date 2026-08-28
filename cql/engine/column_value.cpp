module cql.engine.column_value;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.engine.types;

using namespace plexdb;
using namespace cql::column_types;

namespace plexdb {
    AutoString8 to_str(const cql::column_types::ColumnValue& value, const cql::type::Type& cdtype) {
        if (type_matches_tag<cql::type::Basic>(cdtype.value)) {
            return to_str(value, get<cql::type::Basic>(cdtype.value));
        }
        return visit(value, [&cdtype](auto& v) -> AutoString8 {
            using T = Decay<decltype(v)>;
            if constexpr (SameAs<T, DynamicArray<cql::column_types::NestedColumnValue>>) {
                if (type_matches_tag<cql::type::Tuple>(cdtype.value)) {
                    const auto& t      = get<cql::type::Tuple>(cdtype.value);
                    AutoString8 result = "("_as;
                    for (U64 i = 0; i < v.length; i++) {
                        if (i > 0) {
                            result = result + ", ";
                        }
                        if (i < t.elements.length) {
                            result = result + to_str(v[i].value, t.elements[i]);
                        }
                    }
                    return result + ")";
                }
                if (type_matches_tag<cql::type::UDT*>(cdtype.value)) {
                    cql::type::UDT* u = get<cql::type::UDT*>(cdtype.value);
                    assert_true(u != nullptr, "UDT* column type is null");
                    AutoString8 result = "{"_as;
                    for (U64 i = 0; i < v.length; i++) {
                        if (i > 0) {
                            result = result + ", ";
                        }
                        if (i < u->field_names.length) {
                            result = result + AutoString8(u->field_names[i]) + ": ";
                            result = result + to_str(v[i].value, u->field_types[i]);
                        }
                    }
                    return result + "}";
                }
                bool        is_vec = type_matches_tag<cql::type::Vector>(cdtype.value);
                const auto& elem   = is_vec ? get<cql::type::Vector>(cdtype.value).element : get<cql::type::List>(cdtype.value).element;
                AutoString8 result = "["_as;
                bool        first  = true;
                for (const auto& el : v) {
                    if (!first) {
                        result = result + ", ";
                    }
                    result = result + to_str(el.value, elem);
                    first  = false;
                }
                return result + "]";
            } else if constexpr (SameAs<T, DynamicSet<cql::column_types::NestedColumnValue>>) {
                AutoString8 result = "{"_as;
                bool        first  = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) {
                        result = result + ", ";
                    }
                    result = result + to_str((*it).value, get<cql::type::Set>(cdtype.value).key);
                    first  = false;
                }
                return result + "}";
            } else if constexpr (SameAs<T, DynamicMap<cql::column_types::NestedColumnValue, cql::column_types::NestedColumnValue>>) {
                const auto& m      = get<cql::type::Map>(cdtype.value);
                AutoString8 result = "{"_as;
                bool        first  = true;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!first) {
                        result = result + ", ";
                    }
                    result = result + to_str((*it).first.value, m.key);
                    result = result + ": ";
                    result = result + to_str((*it).second.value, m.value);
                    first  = false;
                }
                return result + "}";
            } else {
                return to_str(cql::column_types::ColumnValue{v}, get<cql::type::Basic>(cdtype.value));
            }
        });
    }
}
