export module plexdb.pager.types;

import plexdb.base;

export namespace plexdb::pager {
    constexpr Array<U8, 6> HEADER_MAGIC{'p', 'X', 'D', 'b', 1, 12};
    constexpr Array<U8, 2> HEADER_CURRENT_VERSION{0, 1};

    // @padding
    struct Header {
        Array<U8, sizeof(HEADER_MAGIC)>           magic;
        Array<U8, sizeof(HEADER_CURRENT_VERSION)> version;
        U64                                       page_size;
        U64                                       page_count;
        U64                                       root_page;
    };
}
