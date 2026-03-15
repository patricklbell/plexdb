module plexdb.blob.paged.detail;

import plexdb.pager;
import plexdb.os;

namespace plexdb::blob {
    // solve (note page alignment means it will always fit U64s):
    //  - x is the data byte count
    //  - h(x) is the header bytes for data bytes x
    // 
    // h(x) := h'(x) + h'(h'(x)) + h'(h'(h'(x))) + ...
    //  - h'(x) is the header entries to store pages of x but not pages for the
    //    header itself.
    // 
    // h'(x) = x*(8/p), note sizeof(U64) = 8
    // so h'(h'(...h'(x)...)) = x*(8/p)^n
    // this is a GP, s = g + g^2 + g^3 + .. = g + gs => s = g/(1 - g)
    // so h(x) = x*g/(1 - g) where g = 8/p
    U64 calculate_header_bytes(U64 x, U64 p) {
        F64 g = static_cast<F64>(sizeof(U64))/p;
        return sizeof(U64)*static_cast<U64>(round_to_infinity(g/(1 - g) * x));
    }

    U64 size(const BlobStaticPaged& blob) {
        return blob.size_bytes;
    }

    template<typename B>
        requires Either<B, const U8, U8>
    TArrayView<B> r_or_rw_block(BlobStaticPaged& blob, U64 offset) {
        const auto& page_size = blob.pager->header.page_size;

        U64 address = offset + blob.header_bytes;
        U64 page_idx = address/page_size;
        U64 offset_in_page = address%page_size;
        
        B* data;
        if constexpr (IsConst<B>) {
            data = pager::rpage(*blob.pager, blob.pages[page_idx]);
        } else {
            data = pager::rwpage(*blob.pager, blob.pages[page_idx]);
        }

        return TArrayView<B>(data + offset_in_page, page_size - offset_in_page);
    }
    TArrayView<const U8> rblock(BlobStaticPaged& blob, U64 offset) {
        return r_or_rw_block<const U8>(blob, offset);
    }
    TArrayView<U8> rwblock(BlobStaticPaged& blob, U64 offset) {
        return r_or_rw_block<U8>(blob, offset);
    }

    void remove_impl(BlobStaticPaged& blob) {
        for (U64 page : blob.pages) {
            delete_page(*blob.pager, page);
        }
    }

    U64 size(const BlobDynamicPaged& blob) {
        return blob.size_bytes;
    }

    template<typename B>
        requires Either<B, const U8, U8>
    TArrayView<B> r_or_rw_block(BlobDynamicPaged& blob, U64 offset) {
        const auto& page_size = blob.pager->header.page_size;

        U64 address = offset + sizeof(U64);
        if (address + sizeof(U64) >= page_size) {
            address += sizeof(U64);
        }
        U64 page_idx = address/page_size;
        U64 offset_in_page = address%page_size;

        assert_true(page_size > offset_in_page, "valid offset");
        U64 length = page_size - offset_in_page;
        // @note exclude trailing header page entry from first data page block
        if (page_idx == 0) {
            assert_true(length > sizeof(U64), "valid offset");
            length -= sizeof(U64);
        }

        B* data;
        if constexpr (IsConst<B>) {
            data = pager::rpage(*blob.pager, blob.data_pages[page_idx]);
        } else {
            data = pager::rwpage(*blob.pager, blob.data_pages[page_idx]);
        }

        return TArrayView<B>(data + offset_in_page, length);
    }
    TArrayView<const U8> rblock(BlobDynamicPaged& blob, U64 offset) {
        return r_or_rw_block<const U8>(blob, offset);
    }
    TArrayView<U8> rwblock(BlobDynamicPaged& blob, U64 offset) {
        return r_or_rw_block<U8>(blob, offset);
    }

    void remove_impl(BlobDynamicPaged& blob) {
        for (U64 page : blob.data_pages) {
            delete_page(*blob.pager, page);
        }
        for (U64 page : blob.header_pages) {
            delete_page(*blob.pager, page);
        }
    }
    void resize_impl(BlobDynamicPaged& blob, U64 new_size) {
        const auto& page_size = blob.pager->header.page_size;

        assert_true(blob.data_pages.length > 0, "must be at least one data page");

        U64 old_size = blob.size_bytes;
        if (new_size == old_size)
            return;
        
        const U64 entry_count_per_page = page_size/sizeof(U64) - 1;
        const U64 next_page_entry_idx = page_size/sizeof(U64) - 1;
        const U64 size_entry_idx = 0;
        assert_true(next_page_entry_idx != size_entry_idx, "size and next page overlap");
        
        const U64 old_data_page_count = blob.data_pages.length;
        const U64 old_header_page_count = blob.header_pages.length;
        const U64 old_total_page_count = old_data_page_count + old_header_page_count;
        
        const U64 new_data_size_bytes = new_size + 2*sizeof(U64);
        const U64 new_data_page_count = ceil_div(new_data_size_bytes, page_size);
        const U64 new_header_page_count = ceil_div(new_data_page_count - 1, entry_count_per_page);
        const U64 new_total_page_count = new_data_page_count + new_header_page_count;

        const U64 first_data_page = blob.data_pages[0];
        U64* first_data_entries = reinterpret_cast<U64*>(pager::rwpage(*blob.pager, first_data_page));

        blob.size_bytes = new_size;
        first_data_entries[size_entry_idx] = new_size;
        if (new_total_page_count == old_total_page_count) {
            return;
        }
        
        assert_true(new_total_page_count > 0, "dynamic blob has at least one page");
        U64* new_pages_ptr = reinterpret_cast<U64*>(os::allocate(sizeof(U64)*new_total_page_count));
        auto new_data_pages = TArrayView(&new_pages_ptr[0], 0_u64);
        auto new_header_pages = (new_header_page_count > 0) ? TArrayView(&new_pages_ptr[new_data_page_count], 0_u64) : TArrayView<U64,U64>();
        
        if (new_total_page_count > old_total_page_count) {
            if (old_data_page_count == 1) {
                U64 first_header_page = pager::new_page(*blob.pager);
                first_data_entries[next_page_entry_idx] = first_header_page;

                append_in_place(new_header_pages, first_header_page);
                append_in_place(new_data_pages, first_data_page);
            // or copy existing header and data pages
            } else {
                os::memory_append_copy(new_data_pages, blob.data_pages);
                os::memory_append_copy(new_header_pages, blob.header_pages);
            }

            // fill in header entries with new data pages, appending header pages
            if (new_header_pages.length <= new_header_page_count) {
                for (;;) {
                    assert_true(new_header_pages.length > 0, "require root header page to build linked list");
                    U64 last_header_page = new_header_pages[new_header_pages.length-1];

                    U64* entries = reinterpret_cast<U64*>(pager::rwpage(*blob.pager, last_header_page));
                    
                    // account for old last header page being partially filled
                    U64 first_entry_idx = 0;
                    if (new_header_pages.length == old_header_page_count) {
                        first_entry_idx = old_data_page_count - (1 + (old_header_page_count-1)*entry_count_per_page);
                        assert_true(first_entry_idx != 0, "zero sized header");
                    }

                    // account for new last header page being partially filled
                    U64 last_entry_count = entry_count_per_page;
                    if (new_header_pages.length == new_header_page_count) {
                        last_entry_count = new_data_page_count - (1 + (new_header_page_count-1)*entry_count_per_page);
                        assert_true(last_entry_count != 0, "zero sized header");
                    }

                    assert_true(first_entry_idx <= last_entry_count, "entry bounds match resizing up");
                    
                    // add new data pages
                    for (U64 entry_idx = first_entry_idx; entry_idx < last_entry_count; entry_idx++) {
                        U64 data_page = pager::new_page(*blob.pager);
                        entries[entry_idx] = data_page;
                        append_in_place(new_data_pages, data_page);
                    }

                    // add next header page
                    if (new_header_pages.length < new_header_page_count) {
                        U64 header_page = pager::new_page(*blob.pager);
                        entries[next_page_entry_idx] = header_page;
                        append_in_place(new_header_pages, header_page);
                    } else {
                        break;
                    }
                }
            }
        } else {
            os::memory_append_copy(new_data_pages, TArrayView(blob.data_pages.ptr, new_data_page_count));

            if (new_header_page_count > 0) {
                os::memory_append_copy(new_header_pages, TArrayView(blob.header_pages.ptr, new_header_page_count));
            }
            
            const U64 new_last_header_idx = max(new_header_page_count, 1_u64) - 1_u64;
            
            for (U64 header_idx = new_last_header_idx; header_idx < old_header_page_count; header_idx++) {
                U64 header_page = blob.header_pages[header_idx];
                U64* entries = reinterpret_cast<U64*>(pager::rwpage(*blob.pager, header_page));
                
                // account for new last header page being partially filled
                U64 first_entry_idx = 0;
                if (header_idx + 1 == new_header_page_count) {
                    first_entry_idx = new_data_page_count - (1 + (new_header_page_count-1)*entry_count_per_page);
                    assert_true(first_entry_idx != 0, "zero sized header");
                }

                // account for old last header page being partially filled
                U64 last_entry_count = entry_count_per_page;
                if (header_idx + 1 == old_header_page_count) {
                    last_entry_count = old_data_page_count - (1 + (old_header_page_count-1)*entry_count_per_page);
                    assert_true(last_entry_count != 0, "zero sized header");
                }

                assert_true(first_entry_idx <= last_entry_count, "entry bounds match resizing down");
                
                for (U64 entry_idx = first_entry_idx; entry_idx < last_entry_count; entry_idx++) {
                    U64 data_page = entries[entry_idx];
                    pager::delete_page(*blob.pager, data_page);
                }
                
                if (header_idx + 1 > new_header_page_count) {
                    pager::delete_page(*blob.pager, header_page);
                }
            }
        }
        
        assert_true(new_data_pages.length == new_data_page_count, "data page view matches calculation");
        assert_true(new_header_pages.length == new_header_page_count, "header page view matches calculation");

        blob.data_pages = new_data_pages;
        blob.header_pages = new_header_pages;
    }
}
