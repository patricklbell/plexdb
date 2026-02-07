export module plexdb.blob.paged.detail;

import plexdb.base;
import plexdb.blob.paged;

export namespace plexdb::blob {
    U64 calculate_header_bytes(U64 x, U64 p);

    U64 size(const BlobStaticPaged& blob);
    TArrayView<const U8> rblock(BlobStaticPaged& blob, U64 offset);
    TArrayView<U8> rwblock(BlobStaticPaged& blob, U64 offset);
    void remove_impl(BlobStaticPaged& blob);

    U64 size(const BlobDynamicPaged& blob);
    TArrayView<const U8> rblock(BlobDynamicPaged& blob, U64 offset);
    TArrayView<U8> rwblock(BlobDynamicPaged& blob, U64 offset);
    void remove_impl(BlobDynamicPaged& blob);
    void resize_impl(BlobDynamicPaged& blob, U64 new_size);
}
