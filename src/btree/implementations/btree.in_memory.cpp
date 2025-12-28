module plexdb.btree.in_memory;

import plexdb.btree.node;
import plexdb.btree.in_memory.detail;

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

    static void deallocate_tree(BTreeInMemory& btree, NodeRef node, CountType depth) {
        const auto& s = get_settings(btree);
        if (depth == s.depth) {
            delete_node(btree, node);
        } else {
            for (const auto& child_ref : children(rnode(btree, node), s))
                deallocate_tree(btree, child_ref, depth + 1);
            delete_node(btree, node);
        }
    }

    BTreeInMemory::~BTreeInMemory() {
        deallocate_tree(*this, get_root(*this), 0);
    }
}