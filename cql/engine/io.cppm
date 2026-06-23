module;
#include <coroutine>

export module cql.engine.io;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;

export namespace cql::io {
    template<typename F>
    concept Read = requires(F f, U8* src, U64 size) {
        { f(src, size) } -> SameAs<coroutine::Task<void>>;
    };

    template<typename F>
    concept Write = requires(F f, const U8* src, U64 size) {
        f(src, size);
    };

    template<typename F>
    concept MarkColumnActive = requires(F f, U64 col_idx) {
        f(col_idx);
    };

    template<typename F>
    concept IsColumnActive = requires(F f, U64 col_idx) {
        f(col_idx);
    };

    // Type-erased concrete IO handles — 16 bytes, pass by value
    using Reader              = plexdb::Functor<coroutine::Task<void>, U8*, U64>;
    using Writer              = plexdb::Functor<void, const U8*, U64>;
    using ColumnActiveSetter  = plexdb::Functor<void, U64>;
    using ColumnActiveChecker = plexdb::Functor<bool, U64>;

    // Bridge helpers: wrap a concept-satisfying callable into the concrete erased handle.
    // The callable must outlive the handle.
    template<Read F>
    Reader to_reader(F& f) {
        return plexdb::to_functor<coroutine::Task<void>, U8*, U64>(f);
    }
    template<Write F>
    Writer to_writer(F& f) {
        return plexdb::to_functor<void, const U8*, U64>(f);
    }
    template<MarkColumnActive F>
    ColumnActiveSetter to_setter(F& f) {
        return plexdb::to_functor<void, U64>(f);
    }
    template<IsColumnActive F>
    ColumnActiveChecker to_checker(F& f) {
        return plexdb::to_functor<bool, U64>(f);
    }

    constexpr U64 COLUMN_COUNT_BYTE_COUNT = sizeof(U64);
    constexpr U64 MASK_BYTE_COUNT         = sizeof(U64);
    constexpr U64 MASK_BIT_COUNT          = MASK_BYTE_COUNT * 8_u64;

    // @note prepended to every row/static blob so the layout is uniform.
    // Bit 0 = HAS_TTL; when set, expiry_unix_ms is the absolute expiry time.
    constexpr U8  ROW_FLAG_HAS_TTL   = 0x01_u8;
    constexpr U64 ROW_METADATA_BYTES = sizeof(U8) + sizeof(S64);

    struct RowMetadata {
        U8  flags          = 0;
        S64 expiry_unix_ms = 0;
    };

    inline bool row_has_ttl(const RowMetadata& m) {
        return (m.flags & ROW_FLAG_HAS_TTL) != 0;
    }
    inline bool row_is_expired(const RowMetadata& m, S64 now_unix_ms) {
        return row_has_ttl(m) && m.expiry_unix_ms <= now_unix_ms;
    }

    void                         write_row_metadata(Writer w, const RowMetadata& m);
    coroutine::Task<RowMetadata> read_row_metadata(Reader r);

    // Declarations — all bodies live in io.cpp
    coroutine::Task<ColumnValue> read_column_value(Reader r, type::Basic dtype);
    coroutine::Task<ColumnValue> read_column_value(Reader r, const type::Type& cdtype);
    coroutine::Task<void>        read_column_mask(Reader r, ColumnActiveSetter mark_active);
    coroutine::Task<void>        skip_column_value(Reader r, type::Basic dtype);
    coroutine::Task<void>        skip_column_value(Reader r, const type::Type& col_type);

    void write_default_column_value(Writer w, type::Basic dtype);
    void write_default_column_value(Writer w, const type::Type& cdtype);

    bool can_cast_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype);
    bool can_write_column_value(const ColumnValue& value, const type::Type& cdtype);

    void write_column_value(Writer w, const ColumnValue& value, const type::Type& cdtype);
    void cast_write_evaluated_as_column_value(Writer w, const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx);
    void cast_write_evaluated_as_column_value(Writer w, const Evaluated& evaluated, const type::Type& cdtype);
    void write_column_mask(Writer w, ColumnActiveChecker is_active, U64 column_count);
}

export namespace plexdb {
    AutoString8 to_str(const cql::ColumnValue& value, cql::type::Basic dtype);
    AutoString8 to_str(const cql::ColumnValue& value, const cql::type::Type& cdtype);
}
