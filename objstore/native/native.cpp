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
}