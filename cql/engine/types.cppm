export module cql.engine.types;

export import cql.column_types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

using namespace plexdb;

export namespace cql {
    namespace type {
        // Re-exposed as a member of this (engine) `type` namespace so `type::Basic` keeps
        // resolving exactly as it did before Basic moved to cql.column_types — its canonical
        // home is `cql::column_types::Basic` now (deliberately NOT nested under a `type` there,
        // to avoid a second same-named namespace colliding with this one), this is just a
        // same-name alias via using-declaration (not a using-directive, so it doesn't reopen
        // the namespace-name collision it's working around).
        using column_types::Basic;

        struct List;
        struct Set;
        struct Map;
        struct Vector;
        struct Tuple;
        struct UDT;

        // @note UDT is held by pointer so all columns referencing the same user-defined type share the
        // canonical definition (Cassandra: ALTER TYPE ADD propagates to every column). Schema.udts owns
        // the pointees with stable addresses across mutations.
        struct Type {
            HybridTaggedUnion<TypeList<Basic>, TypeList<List, Set, Map, Vector, Tuple, UDT*>> value;
        };

        struct List {
            Type element;
            bool frozen;
        };

        struct Set {
            Type key;
            bool frozen;
        };

        struct Map {
            Type key;
            Type value;
            bool frozen;
        };

        struct Vector {
            Type element;
            U64  count;
            bool frozen;
        };

        struct Tuple {
            DynamicArray<Type> elements;
            bool               frozen;
        };

        // @warn the String8 / DynamicArray<String8> fields are non-owning views into the parent
        // UdtStorage's AutoString8 buffers. AutoString8 stores a pointer to a separately-allocated
        // char buffer, so the views remain valid as the owning array grows.
        struct UDT {
            String8               keyspace;
            String8               name;
            DynamicArray<String8> field_names;
            DynamicArray<Type>    field_types;
        };

        bool operator==(const Type& a, const Type& b);

        inline bool operator==(const List& a, const List& b) {
            return a.element == b.element && a.frozen == b.frozen;
        }
        inline bool operator==(const Set& a, const Set& b) {
            return a.key == b.key && a.frozen == b.frozen;
        }
        inline bool operator==(const Map& a, const Map& b) {
            return a.key == b.key && a.value == b.value && a.frozen == b.frozen;
        }
        inline bool operator==(const Vector& a, const Vector& b) {
            return a.element == b.element && a.count == b.count && a.frozen == b.frozen;
        }
        inline bool operator==(const Tuple& a, const Tuple& b) {
            if (a.frozen != b.frozen || a.elements.length != b.elements.length) {
                return false;
            }
            for (U64 i = 0; i < a.elements.length; i++) {
                if (!(a.elements[i] == b.elements[i])) {
                    return false;
                }
            }
            return true;
        }
        inline bool operator==(const UDT& a, const UDT& b) {
            return a.keyspace == b.keyspace && a.name == b.name;
        }

        inline bool operator==(const Type& a, const Type& b) {
            return a.value == b.value;
        }

        inline Type create_basic(Basic d) {
            return Type{Basic{d}};
        }
        inline Type create_list(Basic el, bool frozen = false) {
            return Type{
                List{Type{el}, frozen}
            };
        }
        inline Type create_list(Type el, bool frozen = false) {
            return Type{
                List{move(el), frozen}
            };
        }
        inline Type create_set(Basic key, bool frozen = false) {
            return Type{
                Set{Type{key}, frozen}
            };
        }
        inline Type create_set(Type key, bool frozen = false) {
            return Type{
                Set{move(key), frozen}
            };
        }
        inline Type create_map(Basic key, Basic val, bool frozen = false) {
            return Type{
                Map{Type{key}, Type{val}, frozen}
            };
        }
        inline Type create_map(Type key, Type val, bool frozen = false) {
            return Type{
                Map{move(key), move(val), frozen}
            };
        }
        inline Type create_vector(Basic el, U64 count, bool frozen = false) {
            return Type{
                Vector{Type{el}, count, frozen}
            };
        }
        inline Type create_vector(Type el, U64 count, bool frozen = false) {
            return Type{
                Vector{move(el), count, frozen}
            };
        }
        inline Type create_tuple(DynamicArray<Type> elements, bool frozen = false) {
            return Type{
                Tuple{move(elements), frozen}
            };
        }
        inline Type create_udt(UDT* udt) {
            assert_true(udt != nullptr, "create_udt requires non-null pointer");
            return Type{udt};
        }

        // ====================================================================
        // type ast (parser-only)
        //
        // The parser cannot resolve `[ks.]name` references against the schema, so it emits a
        // separate `ast::Type` tree that mirrors `type::Type` but uses `ast::UdtRef` (a textual
        // [keyspace.]name) wherever `type::Type` would carry a `UDT*`. The engine converts
        // `ast::Type` → `type::Type` via `schema::resolve_type_ast(...)` before any storage or
        // type-checking takes place; resolution looks up the keyspace's UDT pool and binds a
        // stable pointer.
        // ====================================================================
        namespace ast {
            struct ListAst;
            struct SetAst;
            struct MapAst;
            struct VectorAst;
            struct TupleAst;
            struct UdtRef;

            struct Type {
                HybridTaggedUnion<TypeList<Basic>, TypeList<ListAst, SetAst, MapAst, VectorAst, TupleAst, UdtRef>> value;
            };

            struct ListAst {
                Type element;
                bool frozen;
            };
            struct SetAst {
                Type key;
                bool frozen;
            };
            struct MapAst {
                Type key;
                Type value;
                bool frozen;
            };
            struct VectorAst {
                Type element;
                U64  count;
                bool frozen;
            };
            struct TupleAst {
                DynamicArray<Type> elements;
                bool               frozen;
            };
            struct UdtRef {
                AutoString8 keyspace; // may be empty (defaulted to current keyspace at resolve time)
                AutoString8 name;
                bool        frozen;
            };

            inline Type create_basic(Basic d) {
                return Type{Basic{d}};
            }
            inline Type create_list(Type el, bool frozen = false) {
                return Type{
                    ListAst{move(el), frozen}
                };
            }
            inline Type create_set(Type key, bool frozen = false) {
                return Type{
                    SetAst{move(key), frozen}
                };
            }
            inline Type create_map(Type key, Type val, bool frozen = false) {
                return Type{
                    MapAst{move(key), move(val), frozen}
                };
            }
            inline Type create_vector(Type el, U64 count, bool frozen = false) {
                return Type{
                    VectorAst{move(el), count, frozen}
                };
            }
            inline Type create_tuple(DynamicArray<Type> elements, bool frozen = false) {
                return Type{
                    TupleAst{move(elements), frozen}
                };
            }
            inline Type create_udt_ref(AutoString8 keyspace, AutoString8 name, bool frozen = false) {
                return Type{
                    UdtRef{move(keyspace), move(name), frozen}
                };
            }

            inline bool operator==(const Type& a, const Type& b) {
                return a.value == b.value;
            }
            inline bool operator==(const ListAst& a, const ListAst& b) {
                return a.element == b.element && a.frozen == b.frozen;
            }
            inline bool operator==(const SetAst& a, const SetAst& b) {
                return a.key == b.key && a.frozen == b.frozen;
            }
            inline bool operator==(const MapAst& a, const MapAst& b) {
                return a.key == b.key && a.value == b.value && a.frozen == b.frozen;
            }
            inline bool operator==(const VectorAst& a, const VectorAst& b) {
                return a.element == b.element && a.count == b.count && a.frozen == b.frozen;
            }
            inline bool operator==(const TupleAst& a, const TupleAst& b) {
                if (a.frozen != b.frozen || a.elements.length != b.elements.length) {
                    return false;
                }
                for (U64 i = 0; i < a.elements.length; i++) {
                    if (!(a.elements[i] == b.elements[i])) {
                        return false;
                    }
                }
                return true;
            }
            inline bool operator==(const UdtRef& a, const UdtRef& b) {
                return a.keyspace == b.keyspace && a.name == b.name && a.frozen == b.frozen;
            }
        }
    }

    // True when a value asserted as type `from` can be written to a column of type `to`.
    bool type_compatible_for_assignment(type::Basic from, type::Basic to);
}

export namespace plexdb {
    U64 hash(const cql::type::List& t);
    U64 hash(const cql::type::Set& t);
    U64 hash(const cql::type::Map& t);
    U64 hash(const cql::type::Vector& t);
    U64 hash(const cql::type::Tuple& t);
    U64 hash(const cql::type::UDT& t);
    U64 hash(cql::type::UDT* t);
    U64 hash(const cql::type::Type& t);

    AutoString8 to_str(cql::type::Type cdtype);
}
