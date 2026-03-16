export module plexdb.shard.token;

import plexdb.base;
import xxhash;

export namespace plexdb::shard {
    constexpr U64 TOKEN_SEED = 0x706c65786462; // 'plexdb'

    inline U64 token_of(const U8* key, U64 len) {
        return XXHash64::hash(key, len, TOKEN_SEED);
    }

    inline U32 owning_shard(U64 token, U32 shard_count) {
        if (shard_count <= 1) return 0;
        // Uniform mapping: divide token space evenly across shards.
        // Equivalent to: token / (2^64 / shard_count), but avoids overflow
        // by using __uint128_t multiplication.
        return static_cast<U32>((static_cast<__uint128_t>(token) * shard_count) >> 64);
    }

    inline U32 shard_for_key(const U8* key, U64 len, U32 shard_count) {
        return owning_shard(token_of(key, len), shard_count);
    }
}
