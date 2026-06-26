module;
#include "murmur3.h"

export module cql.murmur3;

import plexdb.base;

using namespace plexdb;

export namespace cql::murmur3 {
    struct Hash128 {
        U64 low;
        U64 high;
    };

    inline Hash128 x64_128(const U8* key, S32 len, U32 seed) {
        U64 out[2] = {0, 0};
        MurmurHash3_x64_128(key, len, seed, out);
        return Hash128{out[0], out[1]};
    }

    inline U32 x86_32(const U8* key, S32 len, U32 seed) {
        U32 out = 0;
        MurmurHash3_x86_32(key, len, seed, &out);
        return out;
    }
}
