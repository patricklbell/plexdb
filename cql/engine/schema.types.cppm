export module cql.engine.schema.types;

import plexdb.base;
import plexdb.coroutine;
import plexdb.blob;

import cql.engine.types;
import cql.engine.schema;

using namespace plexdb;

// ============================================================================
// inline type encoding
//
// Used inside columns_blob (per-column type) and udts_blob field records. UDT references
// serialize only the local-keyspace name; the loader resolves against the caller-supplied
// keyspace. See cql/engine/schema.cppm for the byte-level layout description.
// ============================================================================
export namespace cql::schema {
    // Returns the inline serialized byte size of `t`.
    U64 serialized_type_size(const type::Type& t);

    // Writes `t` at *offset and advances *offset by serialized_type_size(t).
    coroutine::Task<void> write_type(blob::BlobDynamicPaged& blob, const type::Type& t, U64* offset);

    // Reads a `type::Type` from *offset, resolving UDT name references against `resolve_ks`.
    // Advances *offset past the consumed bytes. Returns Error::MissingKeyspace /
    // Error::MissingType if a UDT name does not resolve.
    coroutine::Task<Result<type::Type>> read_type(blob::BlobDynamicPaged& blob, Keyspace* resolve_ks, U64* offset);

    // Walks the parser's `ast::Type` and returns an equivalent `type::Type` whose UDT*s
    // point into Schema.udts. `default_ks` resolves UdtRef nodes whose keyspace is empty.
    Result<type::Type> resolve_type_ast(Schema& schema, String8 default_ks, const type::ast::Type& ast);
}
