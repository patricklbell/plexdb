export module cql.column_types;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

using namespace plexdb;

// The physical value shapes a CQL column can hold, and the tag (type::Basic) that selects
// among them. Deliberately independent of cql.engine (no Schema/Table/AST dependency) so that
// cql.native's wire codecs can depend on these shapes directly instead of on the engine as a
// whole. cql.engine.types/cql.engine.column_value re-export this module unchanged, so existing
// `import cql.engine.types;`/`import cql.engine.column_value;` call sites need no changes as
// long as they also add `using namespace cql::column_types;` (this namespace is not itself
// `cql`, so it isn't picked up implicitly the way plain `cql::` members are from code already
// nested under `namespace cql { ... }`).
export namespace cql::column_types {
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

    // @note NOT nested in a `type` sub-namespace (unlike cql::engine::type::Type, the schema
    // tree) — a second namespace also named `type` here would collide with cql::type wherever
    // both get pulled in via `using namespace`, making every `type::X` reference ambiguous.
    // cql.engine.types re-exposes this as `cql::type::Basic` via a using-declaration so existing
    // `type::Basic::X` call sites keep working without touching them.
    // @warn be careful about changing these values as they are encoded directly in the schema's
    // type registry, see schema.cppm
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

    // ========================================================================
    // ColumnValue — the tagged union of every physical shape a column can hold
    // ========================================================================
    struct NestedColumnValue; // @note forward declare for recursive collection types

    // @todo support UDTs
    using ColumnValueBasicTypes = TypeList<
        AutoString8, S64, S32, S16, U8, F64, F32,
        Blob, UUID, Inet, VarInt, Decimal, Duration>;
    using ColumnValueTypes = Concat<
        ColumnValueBasicTypes,
        TypeList<
            DynamicArray<NestedColumnValue>,
            DynamicSet<NestedColumnValue>,
            DynamicMap<NestedColumnValue, NestedColumnValue>>,
        TypeList<Null>>;
    using ColumnValue = ExpandTaggedUnion<ColumnValueTypes>;

    struct NestedColumnValue {
        ColumnValue value;
    };

    bool operator==(const NestedColumnValue& a, const NestedColumnValue& b);
    U64  hash(const NestedColumnValue& ncv);
    S32  compare_column_value(const ColumnValue& a, const ColumnValue& b, Basic dtype);
}

export namespace plexdb {
    U64 hash(const cql::column_types::Basic& t);

    String8     to_str(cql::column_types::Basic dtype);
    AutoString8 to_str(const cql::column_types::ColumnValue& value, cql::column_types::Basic dtype);
}
