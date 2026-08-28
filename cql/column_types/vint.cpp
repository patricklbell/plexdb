module cql.column_types.vint;

import plexdb.base;

import cql.engine.io.types;

using namespace plexdb;

namespace cql::vint {
    static U64 encode_zigzag64(S64 n) {
        return (U64(n) << 1) ^ U64(n >> 63);
    }
    static S64 decode_zigzag64(U64 n) {
        U64 mask = 0ull - (n & 1);
        return S64((n >> 1) ^ mask);
    }

    // `extra` marker bits in the first byte leave (7-extra) value bits there for extra=0..7,
    // plus 8 value bits per following byte; extra=8 is the special all-marker (0xFF) form
    // carrying the full 64-bit value in 8 more bytes.
    static U32 unsigned_size(U64 v) {
        if (v < (1ull << 7)) {
            return 1;
        }
        if (v < (1ull << 14)) {
            return 2;
        }
        if (v < (1ull << 21)) {
            return 3;
        }
        if (v < (1ull << 28)) {
            return 4;
        }
        if (v < (1ull << 35)) {
            return 5;
        }
        if (v < (1ull << 42)) {
            return 6;
        }
        if (v < (1ull << 49)) {
            return 7;
        }
        if (v < (1ull << 56)) {
            return 8;
        }
        return 9;
    }

    U64 encoded_length(S64 value) {
        return unsigned_size(encode_zigzag64(value));
    }

    void write(io::Writer w, S64 value) {
        U64 zz     = encode_zigzag64(value);
        U32 size   = unsigned_size(zz);
        U32 extra  = size - 1;
        U8  buf[9] = {};
        U64 v      = zz;
        for (S32 i = S32(extra); i >= 0; i--) {
            buf[i] = U8(v & 0xFF);
            v >>= 8;
        }
        buf[0] = U8(buf[0] | (~(0xFFu >> extra) & 0xFFu));
        w(buf, size);
    }

    S64 read(const U8*& p, const U8* end) {
        assert_true(p < end, "truncated vint");
        U8  first = *p++;
        U32 extra = 0;
        U8  mask  = 0x80;
        while (extra < 8 && (first & mask)) {
            extra++;
            mask = U8(mask >> 1);
        }
        U64 value = U64(first & U8(0xFFu >> extra));
        assert_true(p + extra <= end, "truncated vint continuation bytes");
        for (U32 i = 0; i < extra; i++) {
            value = (value << 8) | U64(*p++);
        }
        return decode_zigzag64(value);
    }
}
