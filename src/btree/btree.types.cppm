export module plexdb.btree.types;

import plexdb.base;

export namespace plexdb::btree {
    using KeyType = U64;
    using CountType = U16;
    using NodeRef = U64;

    struct Node {
        CountType key_count;
        NodeRef prev, next;
    };

    struct Settings {
        CountType max_keys_per_internal;
        CountType max_keys_per_leaf;
        U64 value_stride;
        U64 depth;
        U64 size;
    };
}