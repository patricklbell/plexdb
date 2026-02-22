module plexdb.blob.paged;

import plexdb.threads;
import plexdb.arena;
import plexdb.os;
import plexdb.blob.paged.detail;

namespace plexdb::blob {
    BlobStaticPaged::BlobStaticPaged(Pager* in_pager, U64 in_page, U64 in_size) {
        const auto& page_size = in_pager->header.page_size;

        this->pager = in_pager;
        this->size_bytes = in_size;
        
        if (this->size_bytes <= page_size) {
            this->header_bytes = 0;
            
            U64* pages_ptr = reinterpret_cast<U64*>(os::allocate(sizeof(U64)));
            pages_ptr[0] = in_page;
            this->pages = TArrayView<U64,U64>(pages_ptr, 1);
            return;
        }

        // @note first header page is implicit
        U64 x = this->size_bytes - page_size;
        U64 h = calculate_header_bytes(x, page_size);
        this->header_bytes = h;

        assert_true(h % sizeof(U64) == 0, "header bytes have correct alignment");

        
        {
            U64 pages_count = 1 + h/sizeof(U64);
            U64* pages_ptr = reinterpret_cast<U64*>(os::allocate(sizeof(U64)*pages_count));
            pages_ptr[0] = in_page;

            U64 h_page_idx = 1;
            U64 h_offset = 0;
            U64 h_page = pages_ptr[0];
            while (h_offset + page_size < h) {
                const U8* data = pager::rpage(*in_pager, h_page);
                os::memory_copy(&pages_ptr[1 + h_offset/sizeof(U64)], data, page_size); // @note skip root

                h_offset += page_size;
                h_page = pages_ptr[h_page_idx];
                h_page_idx++;
            }

            const U8* data = pager::rpage(*in_pager, h_page);
            os::memory_copy(&pages_ptr[1 + h_offset/sizeof(U64)], data, h - h_offset); // @note skip root

            this->pages = TArrayView(pages_ptr, pages_count);
        }
    }

    BlobStaticPaged::BlobStaticPaged(BlobStaticPaged&& other):
        header_bytes(other.header_bytes),
        size_bytes(other.size_bytes),
        pager(other.pager),
        pages(other.pages)
    {
        other.pages.ptr = nullptr;
    }

    BlobStaticPaged& BlobStaticPaged::operator=(BlobStaticPaged&& other) {
        if (this != &other) {
            os::deallocate(this->pages.ptr);

            this->header_bytes = other.header_bytes;
            this->size_bytes   = other.size_bytes;
            this->pager        = other.pager;
            this->pages        = other.pages;

            other.pages.ptr = nullptr;
            other.header_bytes = 0;
            other.size_bytes = 0;
        }

        return *this;
    }

    BlobStaticPaged::~BlobStaticPaged() {
        os::deallocate(this->pages.ptr);
    }

    U64 create_paged_static(Pager& pager, U64 size) {
        const auto& page_size = pager.header.page_size;
        assert_true(page_size >= 2*sizeof(U64), "enough space in page");

        if (size <= page_size) {
            return pager::new_page(pager);
        }

        // @note first header page is implicit
        U64 x = size - page_size;
        U64 h = calculate_header_bytes(x, page_size);
        U64 t = size + h;

        U64 total_page_count = ceil_div(t, page_size);
        assert_true(total_page_count > 0, "expected at least one page");

        U64 root;

        {
            threads::Scope scratch = threads::scratch();
            U64* pages = arena::push_array<U64>(*scratch.arena, total_page_count);

            for (U64 idx = 0; idx < total_page_count; idx++) {
                // @todo bulk?
                pages[idx] = pager::new_page(pager);
            }
            root = pages[0];

            U64 h_offset = 0;
            U64 h_page_idx = 0;
            while (h_offset + page_size < h) {
                U8* data = pager::rwpage(pager, pages[h_page_idx]);
                os::memory_copy(data, &pages[1 + h_offset/sizeof(U64)], page_size); // @note remove root
                h_page_idx++;
                h_offset += page_size;
            }

            U8* data = pager::rwpage(pager, pages[h_page_idx]);
            os::memory_copy(data, &pages[1 + h_offset/sizeof(U64)], h - h_offset); // @note remove root
        }

        return root;
    }

    BlobDynamicPaged::BlobDynamicPaged(Pager* in_pager, U64 in_page) {
        const auto& page_size = in_pager->header.page_size;
        
        this->pager = in_pager;
        
        const U64 entry_count_per_page = page_size/sizeof(U64) - 1;
        const U64 next_page_entry_idx = page_size/sizeof(U64) - 1;
        const U64 size_entry_idx = 0;
        assert_true(next_page_entry_idx != size_entry_idx, "size and next page overlap");
        
        const U64* first_data_entries = reinterpret_cast<const U64*>(pager::rpage(*in_pager, in_page));
        this->size_bytes = first_data_entries[size_entry_idx];
        const U64 data_size_bytes = this->size_bytes + 2*sizeof(U64);
        
        const U64 data_page_count = ceil_div(data_size_bytes, page_size);
        // @note first data page entry is implicit
        const U64 header_page_count = ceil_div(data_page_count - 1, entry_count_per_page);
        const U64 total_page_count = data_page_count + header_page_count;

        assert_true(total_page_count > 0, "dynamic blob has at least one page");
        U64* pages_ptr = reinterpret_cast<U64*>(os::allocate(sizeof(U64)*total_page_count));

        this->data_pages = TArrayView(pages_ptr, 0_u64);
        append_in_place(this->data_pages, in_page);
        
        // @note done if no headers to traverse
        if (header_page_count == 0) {
            this->header_pages = TArrayView<U64, U64>();
            return;
        }
    
        this->header_pages = TArrayView(&pages_ptr[data_page_count], 0_u64);

        // traverse header linked list
        append_in_place(this->header_pages, first_data_entries[next_page_entry_idx]);
        for (;;) {
            U64 header_page = this->header_pages[this->header_pages.length-1];
            const U64* entries = reinterpret_cast<const U64*>(pager::rpage(*in_pager, header_page));

            bool last_header_page = this->header_pages.length >= header_page_count;
            
            // @note handles partially filled last header page
            U64 entry_count = last_header_page ? (data_page_count - this->data_pages.length) : entry_count_per_page;
            os::memory_append_copy(data_pages, TArrayView(entries, entry_count));

            if (!last_header_page) {
                append_in_place(this->header_pages, entries[next_page_entry_idx]);
            } else {
                break;
            }
        }

        assert_true(this->header_pages.length == header_page_count, "header page view matches calculation");
        assert_true(this->data_pages.length == data_page_count, "data page view matches calculation");
    }

    BlobDynamicPaged::BlobDynamicPaged(BlobDynamicPaged&& other):
        size_bytes(other.size_bytes),
        pager(other.pager),
        header_pages(other.header_pages),
        data_pages(other.data_pages)
    {
        other.data_pages.ptr = nullptr;
    }

    BlobDynamicPaged& BlobDynamicPaged::operator=(BlobDynamicPaged&& other) {
        if (this != &other) {
            os::deallocate(this->data_pages.ptr);

            this->size_bytes   = other.size_bytes;
            this->pager        = other.pager;
            this->header_pages = other.header_pages;
            this->data_pages   = other.data_pages;

            other.data_pages.ptr = nullptr;
        }

        return *this;
    }

    BlobDynamicPaged::~BlobDynamicPaged() {
        os::deallocate(this->data_pages.ptr);
    }

    U64 create_paged_dynamic(Pager& pager, U64 initial_size) {
        const auto& page_size = pager.header.page_size;
        assert_true(page_size > 2*sizeof(U64), "enough space in page");

        U64 page = pager::new_page(pager);
        U64* size_ptr = reinterpret_cast<U64*>(pager::rwpage(pager, page));
        *size_ptr = 0;

        // @todo @perf avoid allocations and unnecessary reads
        if (initial_size != 0) {
            BlobDynamicPaged blob{&pager, page};
            resize_impl(blob, initial_size);
        }

        return page;
    }
}