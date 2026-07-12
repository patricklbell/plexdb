export module cql.engine.io.evaluator;

import plexdb.base;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.types;
import cql.engine.statements;
import cql.engine.io.codec;

using namespace plexdb;

export namespace cql::io {
    // @todo cover non-basic targets and string/blob->wide-type coercions; caller falls back to the byte path.
    Optional<ColumnValue> resolve_literal_scalar(const Literal& lit, const type::Type& cdtype);

    // @todo cover tuple/UDT literals; caller falls back to the byte path.
    Optional<ColumnValue> resolve_evaluated(const Evaluated& eval, const type::Type& cdtype, const EvalContext& ctx);

    bool can_write_evaluated_as_column_value(const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx);
    void write_evaluated_as_column_value(Writer w, const Evaluated& evaluated, const type::Type& cdtype, const EvalContext& ctx);
}
