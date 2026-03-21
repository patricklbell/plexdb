module objstore.native;

namespace objstore::native {
    U16 read_be_u16(const U8* p) {
        return (U16(p[0]) << 8) | p[1];
    }

    S16 read_be_s16(const U8* p) {
        return S16(read_be_u16(p));
    }

    S32 read_be_s32(const U8* p) {
        return S32((U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | p[3]);
    }

    S64 read_be_s64(const U8* p) {
        return S64(
            (U64(p[0]) << 56) | (U64(p[1]) << 48) | (U64(p[2]) << 40) | (U64(p[3]) << 32) |
            (U64(p[4]) << 24) | (U64(p[5]) << 16) | (U64(p[6]) << 8)  | U64(p[7])
        );
    }

    // Read [long string]: [int] n + n bytes
    String8 read_cql_long_string(const U8*& p, const U8* end) {
        assert_true(p + 4 <= end, "truncated long string length");
        S32 len = read_be_s32(p);
        p += 4;
        assert_true(len >= 0 && p + len <= end, "long string body truncated");
        String8 s(reinterpret_cast<const char*>(p), U64(len));
        p += len;
        return s;
    }

    // Read [string]: [short] n + n bytes
    String8 read_cql_string(const U8*& p, const U8* end) {
        assert_true(p + 2 <= end, "truncated string length");
        U16 len = read_be_u16(p);
        p += 2;
        assert_true(p + len <= end, "string body truncated");
        String8 s(reinterpret_cast<const char*>(p), U64(len));
        p += len;
        return s;
    }

    // Read [short bytes]: [short] n + n bytes
    U16 read_cql_short_bytes(const U8*& p, const U8* end, const U8*& out_data) {
        assert_true(p + 2 <= end, "truncated short bytes length");
        U16 len = read_be_u16(p);
        p += 2;
        assert_true(p + len <= end, "short bytes body truncated");
        out_data = p;
        p += len;
        return len;
    }
}