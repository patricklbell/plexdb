export module plexdb.btree.types;

import plexdb.base;

export namespace plexdb::btree {
    using KeyType = U64;
    using CountType = U16;
    using NodeRef = U64;

    // @padding
    struct Node {
        CountType key_count;
        NodeRef prev, next;
    };

    // @padding
    struct Header {
        U64 value_stride;
        U64 depth;
        U64 size;
        NodeRef root;
        NodeRef leaves;
        CountType max_keys_per_internal;
        CountType max_keys_per_leaf;
    };
}