module;
#include <string.h>

module cql.engine.token;

import plexdb.base;

using namespace plexdb;

namespace cql::token {
    // @note Cassandra-flavoured Murmur3_x64_128. The body matches Austin
    // Appleby's reference (and the PeterScott vendor in third_party/), but the
    // tail reads sign-extend the bytes because Cassandra reads partition bytes
    // through Java's `byte` (signed) and widens via `(long)` which sign-extends.
    // We keep PeterScott vendored unmodified and ship the Cassandra divergence here.
    static U64 rotl64(U64 x, int r) {
        return (x << r) | (x >> (64 - r));
    }

    static U64 fmix64(U64 k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }

    static U64 load64_le(const U8* p) {
        U64 v;
        memcpy(&v, p, 8);
        return v;
    }

    static constexpr U64 C1 = 0x87c37b91114253d5ULL;
    static constexpr U64 C2 = 0x4cf5ad432745937fULL;

    static void process_block(U64& h1, U64& h2, const U8* block) {
        U64 k1 = load64_le(block + 0);
        U64 k2 = load64_le(block + 8);

        k1 *= C1;
        k1 = rotl64(k1, 31);
        k1 *= C2;
        h1 ^= k1;
        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729ULL;

        k2 *= C2;
        k2 = rotl64(k2, 33);
        k2 *= C1;
        h2 ^= k2;
        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5ULL;
    }

    // Sign-extend through int8 → int64 → uint64, matching Java's
    // `((long) byte_val)` on Cassandra's partitioner.
    static void process_tail(U64& h1, U64& h2, const U8* tail, U64 tail_len) {
        U64 k1 = 0;
        U64 k2 = 0;

        auto sx = [](U8 b) -> U64 {
            return static_cast<U64>(static_cast<S64>(static_cast<S8>(b)));
        };

        switch (tail_len) {
            case 15:
                k2 ^= sx(tail[14]) << 48;
                [[fallthrough]];
            case 14:
                k2 ^= sx(tail[13]) << 40;
                [[fallthrough]];
            case 13:
                k2 ^= sx(tail[12]) << 32;
                [[fallthrough]];
            case 12:
                k2 ^= sx(tail[11]) << 24;
                [[fallthrough]];
            case 11:
                k2 ^= sx(tail[10]) << 16;
                [[fallthrough]];
            case 10:
                k2 ^= sx(tail[9]) << 8;
                [[fallthrough]];
            case 9:
                k2 ^= sx(tail[8]);
                k2 *= C2;
                k2 = rotl64(k2, 33);
                k2 *= C1;
                h2 ^= k2;
                [[fallthrough]];
            case 8:
                k1 ^= sx(tail[7]) << 56;
                [[fallthrough]];
            case 7:
                k1 ^= sx(tail[6]) << 48;
                [[fallthrough]];
            case 6:
                k1 ^= sx(tail[5]) << 40;
                [[fallthrough]];
            case 5:
                k1 ^= sx(tail[4]) << 32;
                [[fallthrough]];
            case 4:
                k1 ^= sx(tail[3]) << 24;
                [[fallthrough]];
            case 3:
                k1 ^= sx(tail[2]) << 16;
                [[fallthrough]];
            case 2:
                k1 ^= sx(tail[1]) << 8;
                [[fallthrough]];
            case 1:
                k1 ^= sx(tail[0]);
                k1 *= C1;
                k1 = rotl64(k1, 31);
                k1 *= C2;
                h1 ^= k1;
                break;
            default:
                break;
        }
    }

    static S64 finalize_hash(U64 h1, U64 h2, U64 total_len) {
        h1 ^= total_len;
        h2 ^= total_len;

        h1 += h2;
        h2 += h1;

        h1 = fmix64(h1);
        h2 = fmix64(h2);

        h1 += h2;
        // h2 += h1; // unused — we only need h1 (the low half) for the token.

        return static_cast<S64>(h1);
    }

    static S64 cassandra_murmur3_x64_128_low(const U8* data, U64 length) {
        const U64 nblocks = length / 16;

        U64 h1 = 0;
        U64 h2 = 0;

        for (U64 i = 0; i < nblocks; i++) {
            process_block(h1, h2, data + i * 16);
        }
        process_tail(h1, h2, data + nblocks * 16, length & 15);

        return finalize_hash(h1, h2, length);
    }

    S64 murmur3_token(const U8* data, U64 length) {
        S64 tok = cassandra_murmur3_x64_128_low(data, length);
        return tok == MIN_TOKEN ? MAX_TOKEN : tok;
    }

    void Murmur3State::update(const U8* data, U64 len) {
        total_len += len;

        if (carry_len > 0) {
            U64 need = 16 - carry_len;
            U64 take = len < need ? len : need;
            memcpy(carry + carry_len, data, take);
            carry_len += take;
            data += take;
            len -= take;
            if (carry_len == 16) {
                process_block(h1, h2, carry);
                carry_len = 0;
            }
        }
        while (len >= 16) {
            process_block(h1, h2, data);
            data += 16;
            len -= 16;
        }
        if (len > 0) {
            memcpy(carry + carry_len, data, len);
            carry_len += len;
        }
    }

    S64 Murmur3State::finalize() {
        process_tail(h1, h2, carry, carry_len);
        S64 tok = finalize_hash(h1, h2, total_len);
        return tok == MIN_TOKEN ? MAX_TOKEN : tok;
    }

    void encode_token_be(U8 out[8], S64 token) {
        U64 bits = static_cast<U64>(token) ^ 0x8000000000000000ULL;
        out[0]   = U8(bits >> 56);
        out[1]   = U8(bits >> 48);
        out[2]   = U8(bits >> 40);
        out[3]   = U8(bits >> 32);
        out[4]   = U8(bits >> 24);
        out[5]   = U8(bits >> 16);
        out[6]   = U8(bits >> 8);
        out[7]   = U8(bits & 0xFFu);
    }

    S64 decode_token_be(const U8* src) {
        U64 bits = 0;
        for (int i = 0; i < 8; i++) {
            bits = (bits << 8) | U64(src[i]);
        }
        return static_cast<S64>(bits ^ 0x8000000000000000ULL);
    }
}
