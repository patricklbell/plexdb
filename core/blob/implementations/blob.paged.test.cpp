#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.pager;
import plexdb.blob;
import plexdb.threads;
import plexdb.arena;

using namespace plexdb;
using namespace plexdb::blob;

// @todo decouple from implementation
// @todo test refreshing blob from database between each operation

TEST_CASE("static blob - update", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 blob_size = 256_u64;

    threads::Scope scratch = threads::scratch();
    U8* data = arena::push_array<U8>(*scratch.arena, blob_size);

    // Initialize blob data to zero
    for (U64 i = 0; i < blob_size; i++)
        data[i] = 0;

    Pager pager(pfile, pager::create(pfile, page_size));
    U64 root_page = create_paged_static(pager, blob_size);
    BlobStaticPaged b(&pager, root_page, blob_size);

    SECTION("full update") {
        for (U64 i = 0; i < blob_size; i++)
            data[i] = i + 1;

        update(b, data, blob_size);

        U8* read_back = arena::push_array<U8>(*scratch.arena, blob_size);
        get(b, read_back, blob_size);

        REQUIRE(TArrayView(read_back, blob_size) == TArrayView(data, blob_size));
    }

    SECTION("partial update with offset") {
        U64 offset = 100;
        U64 length = 50;

        for (U64 i = 0; i < length; i++)
            data[offset + i] = i + 1;

        update(b, data + offset, length, offset);

        U8* read_back = arena::push_array<U8>(*scratch.arena, blob_size);
        get(b, read_back, blob_size);

        for (U64 i = 0; i < blob_size; i++) {
            CAPTURE(i);
            if (i >= offset && i < offset + length)
                REQUIRE(read_back[i] == data[i]);
            else
                REQUIRE(read_back[i] == 0);
        }
    }
}

TEST_CASE("static blob - get", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 blob_size = 256_u64;

    threads::Scope scratch = threads::scratch();
    U8* data = arena::push_array<U8>(*scratch.arena, blob_size);
    U8* read_back = arena::push_array<U8>(*scratch.arena, blob_size);

    for (U64 i = 0; i < blob_size; i++)
        data[i] = i + 1;

    Pager pager(pfile, pager::create(pfile, page_size));
    U64 root_page = create_paged_static(pager, blob_size);
    BlobStaticPaged b(&pager, root_page, blob_size);

    update(b, data, blob_size);

    SECTION("full get") {
        get(b, read_back, blob_size);
        REQUIRE(TArrayView(read_back, blob_size) == TArrayView(data, blob_size));
    }

    SECTION("partial get with offset") {
        U64 offset = 50;
        U64 length = 100;

        get(b, read_back, length, offset);

        for (U64 i = 0; i < length; i++) {
            CAPTURE(i);
            REQUIRE(read_back[i] == data[offset + i]);
        }
    }
}

TEST_CASE("static blob - remove", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 blob_size = 128_u64;

    threads::Scope scratch = threads::scratch();
    Pager pager(pfile, pager::create(pfile, page_size));
    U64 initial_page_count = pager.header.page_count;

    U64 root_page = create_paged_static(pager, blob_size);
    BlobStaticPaged b(&pager, root_page, blob_size);

    U8 dummy_data[128] = {};
    update(b, dummy_data, blob_size);

    SECTION("deletes pages from pager") {
        remove(b);

        REQUIRE(pager.header.page_count == initial_page_count);
    }
}

TEST_CASE("dynamic blob - basic operations", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 initial_size = 256_u64;

    threads::Scope scratch = threads::scratch();
    U8* data = arena::push_array<U8>(*scratch.arena, 1024);

    Pager pager(pfile, pager::create(pfile, page_size));
    U64 root_page = create_paged_dynamic(pager, initial_size);
    BlobDynamicPaged b(&pager, root_page);

    SECTION("update and get") {
        for (U64 i = 0; i < initial_size; i++)
            data[i] = i + 1;

        update(b, data, initial_size);

        U8* read_back = arena::push_array<U8>(*scratch.arena, initial_size);
        get(b, read_back, initial_size);

        for (U64 i = 0; i < initial_size; i++) {
            CAPTURE(i);
            REQUIRE(read_back[i] == data[i]);
        }
    }

    SECTION("partial update with offset") {
        U64 offset = 100;
        U64 length = 50;

        for (U64 i = 0; i < initial_size; i++)
            data[i] = 0;
        for (U64 i = 0; i < length; i++)
            data[offset + i] = i + 1;

        update(b, data, initial_size);

        U8* read_back = arena::push_array<U8>(*scratch.arena, initial_size);
        get(b, read_back, initial_size);

        for (U64 i = 0; i < initial_size; i++) {
            CAPTURE(i);
            if (i >= offset && i < offset + length)
                REQUIRE(read_back[i] == data[i]);
            else
                REQUIRE(read_back[i] == 0);
        }
    }
}

TEST_CASE("dynamic blob - resize up", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 initial_size = 100_u64;

    threads::Scope scratch = threads::scratch();
    U8* data = arena::push_array<U8>(*scratch.arena, 1024);

    Pager pager(pfile, pager::create(pfile, page_size));
    U64 root_page = create_paged_dynamic(pager, initial_size);
    BlobDynamicPaged b(&pager, root_page);

    // Write initial data
    for (U64 i = 0; i < initial_size; i++)
        data[i] = i + 1;
    update(b, data, initial_size);

    SECTION("resize to fit in same pages") {
        U64 new_size = 110_u64;
        resize(b, new_size);

        REQUIRE(b.size_bytes == new_size);

        // Old data should be preserved
        U8* read_back = arena::push_array<U8>(*scratch.arena, new_size);
        get(b, read_back, initial_size);
        REQUIRE(TArrayView(read_back, initial_size) == TArrayView(data, initial_size));
    }

    SECTION("resize requiring new data pages") {
        U64 new_size = 300_u64;
        U64 old_page_count = pager.header.page_count;
        
        resize(b, new_size);

        REQUIRE(b.size_bytes == new_size);
        REQUIRE(pager.header.page_count > old_page_count);

        // Old data should be preserved
        U8* read_back = arena::push_array<U8>(*scratch.arena, new_size);
        get(b, read_back, initial_size);
        REQUIRE(TArrayView(read_back, initial_size) == TArrayView(data, initial_size));

        // Can write to new region
        for (U64 i = initial_size; i < new_size; i++)
            data[i] = i + 1;
        update(b, data + initial_size, new_size - initial_size, initial_size);

        get(b, read_back, new_size);
        REQUIRE(TArrayView(read_back, new_size) == TArrayView(data, new_size));
    }

    SECTION("resize from single data page to multiple") {
        // Start with size that fits in one data page
        U64 single_page_size = 50_u64;
        resize(b, single_page_size);
        
        for (U64 i = 0; i < single_page_size; i++)
            data[i] = i + 1;
        update(b, data, single_page_size);

        // Expand to require header pages
        U64 multi_page_size = 500_u64;
        resize(b, multi_page_size);

        REQUIRE(b.size_bytes == multi_page_size);
        REQUIRE(b.header_pages.length > 0);

        // Old data preserved
        U8* read_back = arena::push_array<U8>(*scratch.arena, multi_page_size);
        get(b, read_back, single_page_size);
        REQUIRE(TArrayView(read_back, single_page_size) == TArrayView(data, single_page_size));
    }

    SECTION("resize requiring new header pages") {
        // Create a size that uses all entries in first header page
        U64 entry_count_per_page = (page_size - sizeof(U64)) / sizeof(U64);
        U64 size_for_full_header = (1 + entry_count_per_page) * page_size;
        U64 size_fo_one_header = size_for_full_header - 2*sizeof(U64);
        
        resize(b, size_fo_one_header);
        REQUIRE(b.header_pages.length == 1);

        // Expand to require second header page
        U64 size_for_two_headers = size_for_full_header + page_size;
        resize(b, size_for_two_headers);
        
        REQUIRE(b.size_bytes == size_for_two_headers);
        REQUIRE(b.header_pages.length == 2);
    }

    SECTION("resize directly to require multiple header pages") {
        // Create a size that uses all entries in first header page
        U64 entry_count_per_page = (page_size - sizeof(U64)) / sizeof(U64);
        U64 size_for_full_header = (1 + entry_count_per_page) * page_size;
        U64 size_for_four_headers = 3*size_for_full_header + page_size  - 2*sizeof(U64);
        
        resize(b, size_for_four_headers);
        
        REQUIRE(b.size_bytes == size_for_four_headers);
        REQUIRE(b.header_pages.length == 4);
    }
}

TEST_CASE("dynamic blob - resize down", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 initial_size = 500_u64;

    threads::Scope scratch = threads::scratch();
    U8* data = arena::push_array<U8>(*scratch.arena, 1024);

    Pager pager(pfile, pager::create(pfile, page_size));
    U64 root_page = create_paged_dynamic(pager, initial_size);
    BlobDynamicPaged b(&pager, root_page);

    // Write initial data
    for (U64 i = 0; i < initial_size; i++)
        data[i] = i + 1;
    update(b, data, initial_size);

    SECTION("resize down within same pages") {
        U64 new_size = 450_u64;
        resize(b, new_size);

        REQUIRE(b.size_bytes == new_size);

        // Preserved data should match
        U8* read_back = arena::push_array<U8>(*scratch.arena, new_size);
        get(b, read_back, new_size);
        REQUIRE(TArrayView(read_back, new_size) == TArrayView(data, new_size));
    }

    SECTION("resize down freeing data pages") {
        U64 new_size = 200_u64;
        U64 old_page_count = pager.header.page_count;
        
        resize(b, new_size);

        REQUIRE(b.size_bytes == new_size);
        REQUIRE(pager.header.page_count < old_page_count);

        // Preserved data should match
        U8* read_back = arena::push_array<U8>(*scratch.arena, new_size);
        get(b, read_back, new_size);
        REQUIRE(TArrayView(read_back, new_size) == TArrayView(data, new_size));
    }

    SECTION("resize down frees header pages") {
        // Expand to require multiple header pages
        U64 entry_count_per_page = (page_size - sizeof(U64)) / sizeof(U64);
        U64 size_for_two_headers = (1 + 2 * entry_count_per_page) * page_size - 2*sizeof(U64);
        
        resize(b, size_for_two_headers);
        REQUIRE(b.header_pages.length == 2);

        // Shrink back to one header page
        U64 size_for_one_header = (1 + entry_count_per_page) * page_size - 2*sizeof(U64);
        resize(b, size_for_one_header);

        REQUIRE(b.size_bytes == size_for_one_header);
        REQUIRE(b.header_pages.length == 1);
    }

    SECTION("resize down to single data page") {
        U64 new_size = 50_u64;
        resize(b, new_size);

        REQUIRE(b.size_bytes == new_size);
        REQUIRE(b.data_pages.length == 1);
        REQUIRE(b.header_pages.length == 0);

        // Preserved data should match
        U8* read_back = arena::push_array<U8>(*scratch.arena, new_size);
        get(b, read_back, new_size);
        REQUIRE(TArrayView(read_back, new_size) == TArrayView(data, new_size));
    }
}

TEST_CASE("dynamic blob - resize edge cases", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 initial_size = 100_u64;

    threads::Scope scratch = threads::scratch();
    Pager pager(pfile, pager::create(pfile, page_size));
    U64 root_page = create_paged_dynamic(pager, initial_size);
    BlobDynamicPaged b(&pager, root_page);

    SECTION("resize to same size is no-op") {
        U64 old_page_count = pager.header.page_count;
        resize(b, initial_size);

        REQUIRE(b.size_bytes == initial_size);
        REQUIRE(pager.header.page_count == old_page_count);
    }

    SECTION("resize to zero") {
        resize(b, 0);

        REQUIRE(b.size_bytes == 0);
        REQUIRE(b.data_pages.length == 1); // Always at least one data page
        REQUIRE(b.header_pages.length == 0);
    }

    SECTION("multiple resizes") {
        resize(b, 200);
        resize(b, 400);
        resize(b, 300);
        resize(b, 100);

        REQUIRE(b.size_bytes == 100);
    }

    SECTION("resize up and down preserves data") {
        U8* data = arena::push_array<U8>(*scratch.arena, 1024);
        for (U64 i = 0; i < initial_size; i++)
            data[i] = i + 1;
        update(b, data, initial_size);

        resize(b, 500);
        resize(b, initial_size);

        U8* read_back = arena::push_array<U8>(*scratch.arena, initial_size);
        get(b, read_back, initial_size);
        REQUIRE(TArrayView(read_back, initial_size) == TArrayView(data, initial_size));
    }
}

TEST_CASE("dynamic blob - remove", "[plexdb.blob.paged]") {
    os::File pfile(os::file_tmp());
    U64 page_size = 128_u64;
    U64 blob_size = 500_u64;

    threads::Scope scratch = threads::scratch();
    Pager pager(pfile, pager::create(pfile, page_size));
    U64 initial_page_count = pager.header.page_count;

    U64 root_page = create_paged_dynamic(pager, blob_size);
    BlobDynamicPaged b(&pager, root_page);

    U8 dummy_data[500] = {};
    update(b, dummy_data, blob_size);

    SECTION("deletes all pages from pager") {
        remove(b);

        REQUIRE(pager.header.page_count == initial_page_count);
    }
}