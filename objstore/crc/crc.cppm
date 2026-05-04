export module objstore.crc;

import plexdb.base;

using namespace plexdb;

namespace objstore::crc {
    // Non-reflected MSB-first CRC-24 lookup table
    consteval auto make_crc24_table() {
        struct Table { U32 data[256]{}; };
        Table t;
        for (int i = 0; i < 256; i++) {
            U32 c = U32(i) << 16;
            for (int j = 0; j < 8; j++) {
                c <<= 1;
                if (c & 0x1000000u) c ^= 0x1974F0Bu;
            }
            t.data[i] = c & 0xFFFFFFu;
        }
        return t;
    }

    // Reflected LSB-first CRC-32/ISO-HDLC lookup table
    consteval auto make_crc32_table() {
        struct Table { U32 data[256]{}; };
        Table t;
        for (int i = 0; i < 256; i++) {
            U32 c = U32(i);
            for (int j = 0; j < 8; j++) {
                if (c & 1u) c = (c >> 1) ^ 0xEDB88320u;
                else c >>= 1;
            }
            t.data[i] = c;
        }
        return t;
    }

    inline constexpr auto CRC24_TABLE = make_crc24_table();
    inline constexpr auto CRC32_TABLE = make_crc32_table();

    // CRC-24/LTE (poly 0x1974F0B, init 0x875060)
    export inline U32 crc24(const U8* data, U64 len, U32 init = 0x875060u) {
        U32 c = init;
        for (U64 i = 0; i < len; i++)
            c = CRC24_TABLE.data[((c >> 16) ^ U32(data[i])) & 0xFFu] ^ ((c << 8) & 0xFFFFFFu);
        return c;
    }

    // CRC-32/ISO-HDLC (reflected poly 0xEDB88320)
    // init is the internal running state (not the final result); default is standard CRC-32/ISO-HDLC.
    export inline U32 crc32(const U8* data, U64 len, U32 init = 0xFFFFFFFFu) {
        U32 c = init;
        for (U64 i = 0; i < len; i++)
            c = CRC32_TABLE.data[(c ^ U32(data[i])) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // CQL v5 payload CRC32 seed
    export inline constexpr U32 CRC32_CQL_V5_INIT = 0xBB88812Cu;
}
