module cql.engine.io.evaluator;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.io.codec;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;
using namespace cql;
using namespace cql::io;

namespace cql::io {
    Optional<ColumnValue> resolve_literal_scalar(const Literal& lit, const type::Type& cdtype) {
        if (!type_matches_tag<type::Basic>(cdtype.value)) {
            return {};
        }
        type::Basic b = get<type::Basic>(cdtype.value);
        return visit(lit.value, [&](const auto& c) -> Optional<ColumnValue> {
            using T = Decay<decltype(c)>;
            // Single source of truth for compatibility — see can_write_typed_basic_as_column_value.
            // Everything below is pure construction: dtype is already known-valid for T.
            if (!can_write_typed_basic_as_column_value<T>(b)) {
                return {};
            }
            if constexpr (SameAs<T, Null>) {
                // @note A null scalar carries no width; it is written via the column mask
                return ColumnValue{Null{}};
            } else if constexpr (SameAs<T, S64>) {
                return narrow_s64_literal(c, b);
            } else if constexpr (SameAs<T, bool>) {
                return ColumnValue{static_cast<U8>(c ? 1 : 0)};
            } else if constexpr (SameAs<T, F64>) {
                return narrow_f64_literal(c, b);
            } else if constexpr (SameAs<T, AutoString8>) {
                return ColumnValue{AutoString8{c}}; // text/ascii/varchar are wire-identical
            } else if constexpr (SameAs<T, UUID>) {
                return ColumnValue{c};
            } else if constexpr (SameAs<T, Blob>) {
                return ColumnValue{c};
            } else if constexpr (SameAs<T, Hex>) {
                // hex columns read back as Blob; carry the raw bytes across.
                Blob out{};
                out.value = c.value;
                return ColumnValue{move(out)};
            } else if constexpr (SameAs<T, Duration>) {
                return ColumnValue{c};
            } else {
                return {}; // Unset — already rejected by the compatibility check above
            }
        });
    }

    Optional<ColumnValue> resolve_evaluated(const Evaluated& eval, const type::Type& cdtype, const EvalContext& ctx) {
        return visit(eval.value, [&](const auto& cv) -> Optional<ColumnValue> {
            using T = Decay<decltype(cv)>;
            if constexpr (SameAs<T, ColumnValue>) {
                return cv;
            } else if constexpr (SameAs<T, Literal>) {
                return resolve_literal_scalar(cv, cdtype);
            } else if constexpr (SameAs<T, ListOrVectorLiteral>) {
                const type::Type* el_type = nullptr;
                if (type_matches_tag<type::List>(cdtype.value)) {
                    el_type = &get<type::List>(cdtype.value).element;
                } else if (type_matches_tag<type::Vector>(cdtype.value)) {
                    el_type = &get<type::Vector>(cdtype.value).element;
                } else {
                    return {};
                }
                DynamicArray<NestedColumnValue> arr{};
                for (U64 i = 0; i < cv.elements.length; i++) {
                    Optional<ColumnValue> ev = resolve_evaluated(evaluate(cv.elements[i], ctx), *el_type, ctx);
                    if (!ev.has_value()) {
                        return {};
                    }
                    push_back(arr, NestedColumnValue{move(*ev)});
                }
                return ColumnValue{move(arr)};
            } else if constexpr (SameAs<T, SetLiteral>) {
                if (!type_matches_tag<type::Set>(cdtype.value)) {
                    return {};
                }
                const type::Type&             key_type = get<type::Set>(cdtype.value).key;
                DynamicSet<NestedColumnValue> s{};
                for (U64 i = 0; i < cv.keys.length; i++) {
                    Optional<ColumnValue> ev = resolve_evaluated(evaluate(cv.keys[i], ctx), key_type, ctx);
                    if (!ev.has_value()) {
                        return {};
                    }
                    insert(s, NestedColumnValue{move(*ev)});
                }
                return ColumnValue{move(s)};
            } else if constexpr (SameAs<T, MapLiteral>) {
                if (!type_matches_tag<type::Map>(cdtype.value)) {
                    return {};
                }
                const auto&                                      m = get<type::Map>(cdtype.value);
                DynamicMap<NestedColumnValue, NestedColumnValue> out{};
                for (U64 i = 0; i < cv.key_values.length; i++) {
                    Optional<ColumnValue> k = resolve_evaluated(evaluate(cv.key_values[i].first, ctx), m.key, ctx);
                    Optional<ColumnValue> v = resolve_evaluated(evaluate(cv.key_values[i].second, ctx), m.value, ctx);
                    if (!k.has_value() || !v.has_value()) {
                        return {};
                    }
                    insert(out, NestedColumnValue{move(*k)}, NestedColumnValue{move(*v)});
                }
                return ColumnValue{move(out)};
            } else if constexpr (SameAs<T, TupleLiteral>) {
                if (!type_matches_tag<type::Tuple>(cdtype.value)) {
                    return {};
                }
                const auto&                     tup = get<type::Tuple>(cdtype.value);
                DynamicArray<NestedColumnValue> arr{};
                for (U64 i = 0; i < tup.elements.length; i++) {
                    if (i < cv.elements.length) {
                        Optional<ColumnValue> ev = resolve_evaluated(evaluate(cv.elements[i], ctx), tup.elements[i], ctx);
                        if (!ev.has_value()) {
                            return {};
                        }
                        push_back(arr, NestedColumnValue{move(*ev)});
                    } else {
                        push_back(arr, NestedColumnValue{ColumnValue{Null{}}});
                    }
                }
                return ColumnValue{move(arr)};
            } else if constexpr (SameAs<T, UdtLiteral>) {
                if (!type_matches_tag<type::UDT*>(cdtype.value)) {
                    return {};
                }
                type::UDT* u = get<type::UDT*>(cdtype.value);
                if (u == nullptr) {
                    return {};
                }
                DynamicArray<NestedColumnValue> arr{};
                for (U64 j = 0; j < u->field_types.length; j++) {
                    const Term* term = nullptr;
                    for (U64 i = 0; i < cv.identifier_values.length; i++) {
                        if (String8(cv.identifier_values[i].first) == u->field_names[j]) {
                            term = &cv.identifier_values[i].second;
                            break;
                        }
                    }
                    if (term == nullptr) {
                        push_back(arr, NestedColumnValue{ColumnValue{Null{}}});
                        continue;
                    }
                    Optional<ColumnValue> ev = resolve_evaluated(evaluate(*term, ctx), u->field_types[j], ctx);
                    if (!ev.has_value()) {
                        return {};
                    }
                    push_back(arr, NestedColumnValue{move(*ev)});
                }
                return ColumnValue{move(arr)};
            } else {
                static_assert(!SameAs<T, T>, "resolve_evaluated: unhandled Evaluated arm");
                return {};
            }
        });
    }

    bool can_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx) {
        return resolve_evaluated(evaluated, cdtype, ctx).has_value();
    }

    void write_evaluated_as_column_value(Writer w, const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx) {
        Optional<ColumnValue> cv = resolve_evaluated(evaluated, cdtype, ctx);
        assert_true(cv.has_value(), "invalid evaluated value for write");
        write_column_value(w, *cv, cdtype);
    }

    void write_evaluated_as_column_value(Writer w, const Evaluated& evaluated, const type::Type& cdtype) {
        write_evaluated_as_column_value(w, evaluated, cdtype, EvalContext{});
    }
}
