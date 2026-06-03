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
    struct UUID {
        static constexpr U64 length = 16_u64;
        Array<U8, length> value;

        bool operator==(const UUID& o) const {
            for (U64 i = 0; i < length; i++)
                if (value[i] != o.value[i]) return false;
            return true;
        }
    };
    struct Hex {
        DynamicArray<U8> value;

        bool operator==(const Hex& o) const {
            if (value.length != o.value.length) return false;
            for (U64 i = 0; i < value.length; i++)
                if (value[i] != o.value[i]) return false;
            return true;
        }
    };
    struct Blob {
        DynamicArray<U8> value;

        bool operator==(const Blob& o) const {
            if (value.length != o.value.length) return false;
            for (U64 i = 0; i < value.length; i++)
                if (value[i] != o.value[i]) return false;
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
        Inet() : is_v6(false) { new (&v4) Array<U8, 4>{}; }
        Inet(const Inet& o) : is_v6(o.is_v6) {
            if (o.is_v6) { new (&v6) Array<U8, 16>(o.v6); }
            else          { new (&v4) Array<U8,  4>(o.v4); }
        }
        Inet(Inet&& o) noexcept : is_v6(o.is_v6) {
            if (o.is_v6) { new (&v6) Array<U8, 16>(move(o.v6)); }
            else          { new (&v4) Array<U8,  4>(move(o.v4)); }
        }
        Inet& operator=(const Inet& o) {
            is_v6 = o.is_v6;
            if (o.is_v6) { v6 = o.v6; } else { v4 = o.v4; }
            return *this;
        }
        Inet& operator=(Inet&& o) noexcept {
            is_v6 = o.is_v6;
            if (o.is_v6) { v6 = move(o.v6); } else { v4 = move(o.v4); }
            return *this;
        }
        bool operator==(const Inet& o) const {
            if (is_v6 != o.is_v6) return false;
            if (is_v6) {
                for (int i = 0; i < 16; i++) if (v6[i] != o.v6[i]) return false;
            } else {
                for (int i = 0; i < 4;  i++) if (v4[i] != o.v4[i]) return false;
            }
            return true;
        }
    };

    struct VarInt {
        bool negative = false;
        DynamicArray<U8> magnitude;  // big-endian bytes
        bool operator==(const VarInt& o) const {
            if (negative != o.negative || magnitude.length != o.magnitude.length) return false;
            for (U64 i = 0; i < magnitude.length; i++) if (magnitude[i] != o.magnitude[i]) return false;
            return true;
        }
    };

    struct Decimal {
        S32 scale = 0;
        VarInt unscaled;
        bool operator==(const Decimal& o) const {
            return scale == o.scale && unscaled == o.unscaled;
        }
    };

    struct Duration {
        S32 months = 0;
        S32 days   = 0;
        S64 nanoseconds = 0;
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
        if (inet.is_v6)
            return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&inet.v6[0]), 16));
        else
            return plexdb::hash(plexdb::String8(reinterpret_cast<const char*>(&inet.v4[0]), 4));
    }

    inline U64 hash(const VarInt& vi) {
        U64 h = plexdb::hash(plexdb::String8(vi.magnitude.ptr, vi.magnitude.length));
        h ^= vi.negative ? 0x1ULL : 0x0ULL;
        return h;
    }

    inline U64 hash(const Decimal& d) {
        U64 h = hash(d.unscaled);
        U8 scale_bytes[4];
        os::memory_copy(scale_bytes, &d.scale, 4);
        h ^= plexdb::hash(plexdb::String8(scale_bytes, 4));
        return h;
    }

    inline U64 hash(const Duration& dur) {
        U8 buf[16];
        os::memory_copy(buf,     &dur.months,      4);
        os::memory_copy(buf + 4, &dur.days,        4);
        os::memory_copy(buf + 8, &dur.nanoseconds, 8);
        return plexdb::hash(plexdb::String8(buf, 16));
    }

    namespace type {
        // @warn be careful about changing these values as they encoded directly in the schema's type registry
        // see schema.cppm
        enum class Basic : U8 {
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

        struct List;
        struct Set;
        struct Map;
        struct Vector;

        struct Type {
            HybridTaggedUnion<TypeList<Basic>, TypeList<List, Set, Map, Vector>> value;
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
            U64 count;
            bool frozen;
        };

        bool operator==(const Type& a, const Type& b);

        inline bool operator==(const List&   a, const List&   b) { return a.element == b.element && a.frozen == b.frozen; }
        inline bool operator==(const Set&    a, const Set&    b) { return a.key == b.key && a.frozen == b.frozen; }
        inline bool operator==(const Map&    a, const Map&    b) { return a.key == b.key && a.value == b.value && a.frozen == b.frozen; }
        inline bool operator==(const Vector& a, const Vector& b) { return a.element == b.element && a.count == b.count && a.frozen == b.frozen; }

        inline bool operator==(const Type& a, const Type& b) { return a.value == b.value; }

        inline Type create_basic(Basic d) { return Type{Basic{d}}; }
        inline Type create_list(Basic el, bool frozen=false) { return Type{List{Type{el}, frozen}}; }
        inline Type create_list(Type el, bool frozen=false) { return Type{List{move(el), frozen}}; }
        inline Type create_set(Basic key, bool frozen=false) { return Type{Set{Type{key}, frozen}}; }
        inline Type create_set(Type key, bool frozen=false) { return Type{Set{move(key), frozen}}; }
        inline Type create_map(Basic key, Basic val, bool frozen=false) { return Type{Map{Type{key}, Type{val}, frozen}}; }
        inline Type create_map(Type key, Type val, bool frozen=false) { return Type{Map{move(key), move(val), frozen}}; }
        inline Type create_vector(Basic el, U64 count, bool frozen=false) { return Type{Vector{Type{el}, count, frozen}}; }
        inline Type create_vector(Type el, U64 count, bool frozen=false) { return Type{Vector{move(el), count, frozen}}; }
    }
}

namespace {
    static inline U64 mix(U64 h, U64 x) {
        h ^= x;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
        return h;
    }
}

export namespace plexdb {
    U64 hash(const cql::type::Type& t);

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

    U64 hash(const cql::type::Type& t) {
        return visit(t.value, [&](const auto& v) {
            return hash(v);
        });
    }

    constexpr inline String8 to_str(cql::type::Basic dtype) {
        switch (dtype) {
            case cql::type::Basic::text:       return "text";
            case cql::type::Basic::int_:       return "int";
            case cql::type::Basic::bigint:     return "bigint";
            case cql::type::Basic::smallint:   return "smallint";
            case cql::type::Basic::counter:    return "counter";
            case cql::type::Basic::timestamp:  return "timestamp";
            case cql::type::Basic::boolean:    return "boolean";
            case cql::type::Basic::float_:     return "float";
            case cql::type::Basic::double_:    return "double";
            case cql::type::Basic::uuid:       return "uuid";
            case cql::type::Basic::ascii:      return "ascii";
            case cql::type::Basic::blob:       return "blob";
            case cql::type::Basic::date:       return "date";
            case cql::type::Basic::decimal:    return "decimal";
            case cql::type::Basic::duration:   return "duration";
            case cql::type::Basic::inet:       return "inet";
            case cql::type::Basic::time:       return "time";
            case cql::type::Basic::timeuuid:   return "timeuuid";
            case cql::type::Basic::tinyint:    return "tinyint";
            case cql::type::Basic::varchar:    return "varchar";
            case cql::type::Basic::varint:     return "varint";
            case cql::type::Basic::hex:        return "hex";
        }
        return "unknown";
    }

    // @todo separate cases into functions, same as hash
    inline AutoString8 to_str(cql::type::Type cdtype) {
        return visit(cdtype.value, [](const auto& v) -> AutoString8 {
            using T = RemoveCVRef<decltype(v)>;
            if constexpr (SameAs<T, cql::type::Basic>)
                return AutoString8(to_str(v));
            else if constexpr (SameAs<T, cql::type::List>)
                return "list<"_as + to_str(v.element) + ">";
            else if constexpr (SameAs<T, cql::type::Set>)
                return "set<"_as + to_str(v.key) + ">";
            else if constexpr (SameAs<T, cql::type::Map>)
                return "map<"_as + to_str(v.key) + ", " + to_str(v.value) + ">";
            else {
                static_assert(SameAs<T, cql::type::Vector>, "unhandled Type variant in to_str");
                return "vector["_as + to_str(v.element) + "]";
            }
        });
    }
}
