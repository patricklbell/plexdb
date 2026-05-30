module cql.engine.io;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.evaluator;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;

namespace cql::io {
    bool can_cast_write_constant_as_column_value(const Constant& constant, type::Basic dtype) {
        return visit(constant.value, [&](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            return can_write_typed_basic_as_column_value<T>(dtype);
        });
    }

    bool can_cast_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype) {
        return visit(evaluated.value, [&cdtype](const auto& cv) -> bool {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, Constant>) {
                return type_matches_tag<type::Basic>(cdtype.value) &&
                       can_cast_write_constant_as_column_value(cv, get<type::Basic>(cdtype.value));
            } else if constexpr (SameAs<T, MapLiteral>) {
                return type_matches_tag<type::Map>(cdtype.value);
            } else if constexpr (SameAs<T, SetLiteral>) {
                return type_matches_tag<type::Set>(cdtype.value);
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                return type_matches_tag<type::List>(cdtype.value) || type_matches_tag<type::Vector>(cdtype.value);
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
}
