module plexdb.btree.in_memory;

import plexdb.btree.node;

namespace plexdb::btree {
    BTreeInMemory::BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride) {
        assert_true(max_keys_per_internal >= 3, "positive min keys.");
        assert_true(max_keys_per_leaf >= 3, "positive min keys.");
        
        this->settings = Settings{
            .max_keys_per_internal = max_keys_per_internal,
            .max_keys_per_leaf = max_keys_per_leaf,
            .value_stride = value_stride,
            .depth = 0,
            .size = 0,
        };
        this->leaves = push_leaf_node(this->settings);
        this->root = this->leaves;
    }
}