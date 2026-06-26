export module cql.engine.token;

import plexdb.base;

using namespace plexdb;

export namespace cql::token {
    // @note Cassandra reserves S64::min as MIN_TOKEN, so the partitioner remaps
    // any colliding hash output to S64::max. Token range is [S64::min+1, S64::max].
    constexpr S64 MIN_TOKEN = static_cast<S64>(0x8000000000000000ULL);
    constexpr S64 MAX_TOKEN = static_cast<S64>(0x7FFFFFFFFFFFFFFFLL);

    // Cassandra's Murmur3Partitioner — Murmur3_x64_128 with seed 0, returning
    // the low 64 bits as a signed token. Differs from the canonical PeterScott
    // implementation by sign-extending the tail bytes (Cassandra's Java code
    // reads partition bytes as `byte` which sign-extends on widening to `long`).
    S64 murmur3_token(const U8* data, U64 length);

    // Encode an S64 token as 8 big-endian bytes with the sign bit flipped, so
    // byte-wise lex compare over the encoding matches signed compare on the token.
    void encode_token_be(U8 out[8], S64 token);

    // Inverse of encode_token_be.
    S64 decode_token_be(const U8* src);
}
