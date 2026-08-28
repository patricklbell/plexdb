export module cql.engine.literal_widen;

import plexdb.base;
import cql.column_types;

using namespace plexdb;
using namespace cql;
using namespace cql::column_types;

// Conversions used only to widen a generic CQL literal (S64/F64/AutoString8 — see
// statements.cppm's LiteralTypes) into a column_types value that has no literal syntax of its
// own. Cassandra reuses the integer/float/string constant grammar for varint/decimal/inet
// (cassandra.apache.org/doc/4.1/cassandra/cql/types.html "Constants supported"); these
// functions implement that widening. Consumed by io.evaluator.cpp (resolve_literal_scalar) and
// evaluator.cpp (unary minus re-narrowing) — an engine concern, not a property of the types
// themselves, so this stays out of cql.column_types.
export namespace cql {
    VarInt  varint_from_s64(S64 v);
    Decimal decimal_from_s64(S64 v);
    // Truncates to the low 64 bits of the magnitude (sign-applied), matching the wraparound
    // behavior CQL integer literals already had before arbitrary-precision parsing: an
    // out-of-range literal narrowed into an int-family column wraps mod 2^64, it doesn't error.
    S64 s64_from_varint_truncating(const VarInt& v);
    // Exact (non-truncating) conversion — empty iff v doesn't fit in S64. Used to re-narrow a
    // VarInt back to S64 after an operation (e.g. unary minus) that may bring it back in range.
    Optional<S64> try_s64_from_varint(const VarInt& v);
    // Round-trips v through its shortest decimal representation (e.g. 19.99, not the raw binary
    // fraction) rather than reproducing IEEE-754 noise; still IEEE-754-precision-limited overall
    // since the source is an F64, not a Cassandra-exact literal.
    Decimal decimal_from_f64(F64 v);

    // Parses a dotted-quad IPv4 or colon-hex IPv6 address string. Cassandra has no inet
    // constant syntax; inet values are always written as CQL string constants and coerced by
    // the target column's declared type at bind/insert time.
    Optional<Inet> inet_from_string(String8 s);
}
