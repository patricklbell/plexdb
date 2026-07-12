export module cql.engine.types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

using namespace plexdb;

export namespace cql {
    // ========================================================================
    // value types
    // ========================================================================
    struct Null {};
    // @note CQL native protocol [bytes] length -2 = UNSET; distinct from NULL — UNSET means
    // "no change" for INSERT/UPDATE assignments; NULL means "remove the value".
    struct Unset {};
    struct UUID {
        static constexpr U64 length = 16_u64;
        Array<U8, length>    value;

        bool operator==(const UUID& o) const {
            for (U64 i = 0; i < length; i++) {
                if (value[i] != o.value[i]) {
                    return false;
                }
            }
            return true;
        }
    };
    struct Hex {
        DynamicArray<U8> value;

        bool operator==(const Hex& o) const {
            if (value.length != o.value.length) {
                return false;
            }
            for (U64 i = 0; i < value.length; i++) {
                if (value[i] != o.value[i]) {
                    return false;
                }
            }
            return true;
        }
    };
    struct Blob {
        DynamicArray<U8> value;

        bool operator==(const Blob& o) const {
            if (value.length != o.value.length) {
                return false;
            }
            for (U64 i = 0; i < value.length; i++) {
                if (value[i] != o.value[i]) {
                    return false;
                }
            }
            return true;
        }
    };

    struct Inet {
        bool is_v6;
        union {
            Array<U8, 4>  v4;
            Array<U8, 16> v6;
        };
        // Anonymous union members have non-trivial constructors so we must define all special members
        Inet()
            : is_v6(false) {
            new (&v4) Array<U8, 4>{};
        }
        Inet(const Inet& o)
            : is_v6(o.is_v6) {
            if (o.is_v6) {
                new (&v6) Array<U8, 16>(o.v6);
            } else {
                new (&v4) Array<U8, 4>(o.v4);
            }
        }
        Inet(Inet&& o) noexcept
            : is_v6(o.is_v6) {
            if (o.is_v6) {
                new (&v6) Array<U8, 16>(move(o.v6));
            } else {
                new (&v4) Array<U8, 4>(move(o.v4));
            }
        }
        Inet& operator=(const Inet& o) {
            is_v6 = o.is_v6;
            if (o.is_v6) {
                v6 = o.v6;
            } else {
                v4 = o.v4;
            }
            return *this;
        }
        Inet& operator=(Inet&& o) noexcept {
            is_v6 = o.is_v6;
            if (o.is_v6) {
                v6 = move(o.v6);
            } else {
                v4 = move(o.v4);
            }
            return *this;
        }
        bool operator==(const Inet& o) const {
            if (is_v6 != o.is_v6) {
                return false;
            }
            if (is_v6) {
                for (int i = 0; i < 16; i++) {
                    if (v6[i] != o.v6[i]) {
                        return false;
                    }
                }
            } else {
                for (int i = 0; i < 4; i++) {
                    if (v4[i] != o.v4[i]) {
                        return false;
                    }
                }
            }
            return true;
        }
    };

    struct VarInt {
        bool             negative = false;
        DynamicArray<U8> magnitude; // big-endian bytes
        bool             operator==(const VarInt& o) const {
            if (negative != o.negative || magnitude.length != o.magnitude.length) {
                return false;
            }
            for (U64 i = 0; i < magnitude.length; i++) {
                if (magnitude[i] != o.magnitude[i]) {
                    return false;
                }
            }
            return true;
        }
    };

    struct Decimal {
        S32    scale = 0;
        VarInt unscaled;
        bool   operator==(const Decimal& o) const {
            return scale == o.scale && unscaled == o.unscaled;
        }
    };

    struct Duration {
        S32  months      = 0;
        S32  days        = 0;
        S64  nanoseconds = 0;
        bool operator==(const Duration& o) const {
            return months == o.months && days == o.days && nanoseconds == o.nanoseconds;
        }
    };

    inline U64 hash(const UUID& uuid) {
        return plexdb::hash(plexdb::String8(&uuid.value[0], uuid.length));
    }

    inline U64 hash(const Blob& blob) {
        return plexdb::hash(plexdb::String8(blob.value.ptr, blob.value.length));
    }

    inline U64 hash(const Hex& hex) {
        return plexdb::hash(plexdb::String8(hex.value.ptr, hex.value.length));
    }

    inline U64 hash(const Inet& inet) {
        if (inet.is_v6) {
            return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&inet.v6[0]), 16));
        } else {
            return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&inet.v4[0]), 4));
        }
    }

    inline U64 hash(const VarInt& vi) {
        U64 h = plexdb::hash(plexdb::String8(vi.magnitude.ptr, vi.magnitude.length));
        h ^= vi.negative ? 0x1ULL : 0x0ULL;
        return h;
    }

    inline U64 hash(const Decimal& d) {
        U64 h = hash(d.unscaled);
        U8  scale_bytes[4];
        os::memory_copy(scale_bytes, &d.scale, 4);
        h ^= plexdb::hash(plexdb::String8(scale_bytes, 4));
        return h;
    }

    inline U64 hash(const Duration& dur) {
        U8 buf[16];
        os::memory_copy(buf, &dur.months, 4);
        os::memory_copy(buf + 4, &dur.days, 4);
        os::memory_copy(buf + 8, &dur.nanoseconds, 8);
        return plexdb::hash(plexdb::String8(buf, 16));
    }

    namespace type {
        // @warn be careful about changing these values as they encoded directly in the schema's type registry
        // see schema.cppm
        enum class Basic : U8 {
            ascii = 0,
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
            COUNT,
        };

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
    U64 hash(const cql::type::Basic& t);
    U64 hash(const cql::type::List& t);
    U64 hash(const cql::type::Set& t);
    U64 hash(const cql::type::Map& t);
    U64 hash(const cql::type::Vector& t);
    U64 hash(const cql::type::Tuple& t);
    U64 hash(const cql::type::UDT& t);
    U64 hash(cql::type::UDT* t);
    U64 hash(const cql::type::Type& t);

    String8     to_str(cql::type::Basic dtype);
    AutoString8 to_str(cql::type::Type cdtype);
}
