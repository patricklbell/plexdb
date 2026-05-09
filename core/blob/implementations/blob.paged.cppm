export module plexdb.blob.paged;

import plexdb.base;
import plexdb.coroutine;
import plexdb.pager;

export namespace plexdb::blob {
    // @todo partial blob header loading?

    // A static blob is divided into two parts, the header and body. The header
    // contains a list of all the pages the blob covers (including the header).
    // If there is only one page then there is no header.
    //
    // +------------+------------------------------+
    // |   header   |          ...body...          |
    // +------------+------------------------------+
    //
    // The header and body can cross page boudaries
    // +----+----+----+----+------------------+----+
    // | p1 | p2 | p3 | p4 |       ...        | pN |
    // +----+----+----+----+------------------+----+
    // *note pages do not have to be contiguous
    //
    // This allows offset reads without traversing any extra pages.
    struct BlobStaticPaged {
        Pager* pager = nullptr;
        U64 size_bytes;
        U64 header_bytes;
        TArrayView<U64, U64> pages;

        BlobStaticPaged() = default;
        BlobStaticPaged(BlobStaticPaged&& other);

        BlobStaticPaged& operator=(BlobStaticPaged&& other);

        ~BlobStaticPaged();

        BlobStaticPaged(const BlobStaticPaged& other) = delete;
        BlobStaticPaged& operator=(const BlobStaticPaged& other) = delete;

        explicit operator bool() const { return pager == nullptr; }

        static coroutine::Task<BlobStaticPaged> load(Pager* in_pager, U64 in_page, U64 in_size);
    };
    coroutine::Task<U64> create_paged_static(Pager& pager, U64 size);

    // A dynamic blob is divided into two parts, the header and body. The header
    // (if present) is a singly linked list, with header containing as entries
    // for the data pages, in order from 1 -> n. The first data page is special
    // and contains in the first U64 the size, and last U64 the page of the first
    // header.
    //
    // +----+----+-----+----+----+----+-----+----+----+
    // | d0 | h1 | ... | h3 | d1 | d2 | ... | d3 | d4 |
    // +----+----+-----+----+----+----+-----+----+----+
    // *note pages do not have to be contiguous, this ordering is conceptual
    //
    // This allows appending/truncating without copying or shifting, while
    // preserving offset reads. Disadvantage in that reading offset into large
    // blob requires reading first data page.
    struct BlobDynamicPaged {
        Pager* pager;
        U64 size_bytes;
        TArrayView<U64, U64> header_pages;
        TArrayView<U64, U64> data_pages;

        BlobDynamicPaged() = default;
        BlobDynamicPaged(BlobDynamicPaged&& other);

        BlobDynamicPaged& operator=(BlobDynamicPaged&& other);

        ~BlobDynamicPaged();

        // @todo
        BlobDynamicPaged(const BlobDynamicPaged& other) = delete;
        BlobDynamicPaged& operator=(const BlobDynamicPaged& other) = delete;

        static coroutine::Task<BlobDynamicPaged> load(Pager* in_pager, U64 in_page);
    };
    coroutine::Task<U64> create_paged_dynamic(Pager& pager, U64 initial_size = 0);
}
