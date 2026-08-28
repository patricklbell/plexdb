export module cql.column_types.twos_complement;

import plexdb.base;
import plexdb.dynamic.containers;

import cql.engine.io.types;

using namespace plexdb;

// Minimal-length two's complement byte encoding — the CQL native protocol's `varint` wire
// shape (cassandra.apache.org/doc/latest/cassandra/reference/native-protocol.html §5.24), and
// (composed with a scale) `decimal`'s. Also what Cassandra's partition-key token hashing uses
// for these two types, since token computation hashes each column's decomposed wire bytes,
// independent of client protocol version.
//
// Deliberately a plain bool+bytes API, not VarInt-shaped: this lets both cql.engine (partition
// token hashing) and cql.native (client wire values) call it directly without either depending
// on the other — VarInt itself lives in cql.column_types, a peer of both.
export namespace cql::twos_complement {
    U64  length(bool negative, const U8* magnitude, U64 magnitude_len);
    void write(io::Writer w, bool negative, const U8* magnitude, U64 magnitude_len);

    struct Decoded {
        bool             negative;
        DynamicArray<U8> magnitude;
    };
    Decoded read(const U8* data, U64 len);
}
