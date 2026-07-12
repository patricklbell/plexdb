// GENERATED FILE — do not edit by hand.
// Produced by cql/engine/codegen/generate.py from cql/engine/codegen/wire_types.json.
// Re-run that script after editing the spec; do not hand-patch this file.

export module cql.engine.token.wire;

import cql.engine.io.codec;

import plexdb.base;

import cql.engine.column_value;
import cql.engine.types;

using namespace plexdb;

export namespace cql::token {
    // Cassandra network wire-format encoding of one partition-key component, fed directly
    // into a Writer — used only as Murmur3 token input, never persisted. Composite
    // components (a multi-column partition key) carry a U16 BE length prefix and a 0x00
    // separator; single components are raw bytes. Exhaustive over token-eligible
    // type::Basic (see wire_types.json's token_eligible) — a new partition-key-capable
    // type without a token_eligible entry hits the assert_not_implemented fallback instead
    // of silently producing wrong bytes.
    void write_component(io::Writer w, const ColumnValue& cv, type::Basic dtype, bool is_composite);
}
