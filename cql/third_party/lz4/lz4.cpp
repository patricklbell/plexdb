module;
#include <lz4.h>

module cql.lz4;

import plexdb.base;

using namespace plexdb;

namespace cql::lz4 {
    S32 compress(const U8* src, U8* dst, S32 src_size, S32 dst_capacity) {
        return LZ4_compress_default(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dst),
            src_size, dst_capacity
        );
    }

    S32 decompress(const U8* src, U8* dst, S32 compressed_size, S32 max_decompressed_size) {
        return LZ4_decompress_safe(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(dst),
            compressed_size, max_decompressed_size
        );
    }

    S32 compress_bound(S32 src_size) {
        return LZ4_compressBound(src_size);
    }
}
