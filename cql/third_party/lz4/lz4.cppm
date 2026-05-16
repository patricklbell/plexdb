export module cql.lz4;

import plexdb.base;

using namespace plexdb;

export namespace cql::lz4 {
    // Returns the compressed data size, or 0 on failure.
    // dst must be at least compress_bound(src_size) bytes.
    S32 compress(const U8* src, U8* dst, S32 src_size, S32 dst_capacity);

    // Returns the decompressed data size, or a negative value on failure.
    S32 decompress(const U8* src, U8* dst, S32 compressed_size, S32 max_decompressed_size);

    // Returns the maximum output buffer size for LZ4_compress_default.
    S32 compress_bound(S32 src_size);
}
