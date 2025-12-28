export module plexdb.btree.in_memory;

import plexdb.base;
import plexdb.btree.types;

export namespace plexdb::btree {
    struct BTreeInMemory {
        Node* root = nullptr;
        Node* leaves = nullptr;

        Settings settings;

        explicit BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride);
        ~BTreeInMemory();
    };
}