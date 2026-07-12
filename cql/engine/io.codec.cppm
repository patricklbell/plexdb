module;
#include <coroutine>

export module cql.engine.io.codec;

// .core is hand-written Reader/Writer plumbing; .wire is generated from wire_types.json
// (write_typed_basic, can_write_typed_basic_as_column_value, narrow_s64_literal/
// narrow_f64_literal). Both re-exported here for cql.engine.io.evaluator's use.
export import cql.engine.io.types;
export import cql.engine.io.codec.wire;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.dynamic.tagged_union;
import plexdb.coroutine;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

export namespace cql::io {
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

    // @note Variable-width: flags is always present; expiry/writetime are present iff
    // their bit is set. The cell_meta_mask in the row header decides which columns
    // have a CellMetadata at all; absence falls back to the row's RowMetadata.
    constexpr U8 CELL_FLAG_HAS_TTL       = 0x01_u8;
    constexpr U8 CELL_FLAG_HAS_WRITETIME = 0x02_u8;

    struct CellMetadata {
        U8  flags          = 0;
        S64 expiry_unix_ms = 0;
        S64 writetime_us   = 0;
    };

    inline bool cell_has_ttl(const CellMetadata& m) {
        return (m.flags & CELL_FLAG_HAS_TTL) != 0;
    }
    inline bool cell_has_writetime(const CellMetadata& m) {
        return (m.flags & CELL_FLAG_HAS_WRITETIME) != 0;
    }

    void                          write_cell_metadata(Writer w, const CellMetadata& m);
    coroutine::Task<CellMetadata> read_cell_metadata(Reader r);
    coroutine::Task<void>         skip_cell_metadata(Reader r, U8 flags);

    // Writes a ceil(column_count/MASK_BIT_COUNT)-word bitmask, one bit per column, set
    // whenever is_set(idx) is true — the raw form shared by the column-active mask (see
    // write_column_mask, which prefixes it with a column count) and the cell-meta mask
    // (no prefix; the row's mask already carries the count).
    void write_mask_bits(Writer w, ColumnActiveChecker is_set, U64 column_count);

    coroutine::Task<ColumnValue> read_column_value(Reader r, type::Basic dtype);
    coroutine::Task<ColumnValue> read_column_value(Reader r, const type::Type& cdtype);
    coroutine::Task<void>        read_column_mask(Reader r, ColumnActiveSetter mark_active);
    coroutine::Task<void>        skip_column_value(Reader r, type::Basic dtype);
    coroutine::Task<void>        skip_column_value(Reader r, const type::Type& col_type);

    // Synchronous, allocation-free counterpart to read_column_value — for callers that
    // cannot suspend a coroutine (e.g. a noexcept BTree comparator).
    ColumnValue read_column_value_sync(const U8* src, U16 len, U16* pos, type::Basic dtype);

    void write_default_column_value(Writer w, type::Basic dtype);
    void write_default_column_value(Writer w, const type::Type& cdtype);

    bool can_write_column_value(const ColumnValue& value, const type::Type& cdtype);
    void write_column_value(Writer w, const ColumnValue& value, const type::Type& cdtype);
    void write_column_mask(Writer w, ColumnActiveChecker is_active, U64 column_count);
}
