export module plexdb.btree.paged;

import plexdb.base;
import plexdb.os;
import plexdb.btree.types;
import plexdb.pager;

export namespace plexdb::btree {
    struct BTreePaged {
        Pager* pager;
        U64 root = ~0u;
        U64 leaves = ~0u;

        Settings settings;

        explicit BTreePaged(Pager* pager, U64 base_page);
    };
}