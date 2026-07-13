export module cql.engine.io.evaluator;

import plexdb.base;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.types;
import cql.engine.statements;
import cql.engine.io.codec;

using namespace plexdb;

export namespace cql::io {
    // Cassandra's cap on a single partition/clustering key value: 0xFFFF, the largest
    // length a 16-bit prefix can encode (the key wire formats use one). Collection
    // elements are deliberately NOT capped here — Cassandra's own 64K limit on those is
    // narrower and stranger than "always reject" (scylladb/scylladb#7745: map keys and
    // set elements over 64K are rejected only in unprepared statements; list elements
    // and prepared-statement bind values aren't limited at all), so getting it exactly
    // right isn't worth it — see cql-conformance-status.md's "Single-node considerations".
    constexpr U64 MAX_CQL_KEY_VALUE_BYTES = 65535;

    // True if `eval` resolves to a text/blob/hex literal longer than MAX_CQL_KEY_VALUE_BYTES.
    // Callers use this to reject an oversized value bound for a partition or clustering
    // key column; false for every other Evaluated shape (collections, non-key literals).
    bool evaluated_exceeds_max_key_size(const Evaluated& eval);

    // @todo cover non-basic targets and string/blob->wide-type coercions; caller falls back to the byte path.
    Optional<ColumnValue> resolve_literal_scalar(const Literal& lit, const type::Type& cdtype);

    // @todo cover tuple/UDT literals; caller falls back to the byte path.
    Optional<ColumnValue> resolve_evaluated(const Evaluated& eval, const type::Type& cdtype, const EvalContext& ctx);

    bool can_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx);
    void write_evaluated_as_column_value(Writer w, const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx);
}
