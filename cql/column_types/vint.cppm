export module cql.column_types.vint;

import plexdb.base;

import cql.engine.io.types;

using namespace plexdb;

// Cassandra's VIntCoding: zigzag-encode a signed value, then a variable-length unsigned
// encoding whose first byte's leading run of 1-bits (before the first 0-bit) counts the extra
// bytes that follow (0 to 8; 9 bytes total covers the full 64-bit range). Used by the CQL
// native protocol's `duration` type ([vint] months, [vint] days, [vint] nanoseconds — see
// cassandra.apache.org/doc/latest/cassandra/reference/native-protocol.html §3, §5.8) and by
// nothing else today, but the encoding itself is a generic signed-integer wire primitive, not
// duration-specific — kept independent of cql::Duration for the same reason twos_complement is
// independent of cql::VarInt.
export namespace cql::vint {
    U64  encoded_length(S64 value);
    void write(io::Writer w, S64 value);
    S64  read(const U8*& p, const U8* end);
}
