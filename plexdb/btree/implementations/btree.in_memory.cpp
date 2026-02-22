module plexdb.btree.in_memory;

import plexdb.os;
import plexdb.btree.node;
import plexdb.btree.in_memory.detail;

namespace plexdb::btree {
    BTreeInMemory::BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride) {
        assert_true(max_keys_per_internal >= 3, "positive min keys.");
        assert_true(max_keys_per_leaf >= 3, "positive min keys.");
        
        this->header = Header{
            .value_stride = value_stride,
            .depth = 0,
            .size = 0,
            .max_keys_per_internal = max_keys_per_internal,
            .max_keys_per_leaf = max_keys_per_leaf,
        };
        this->header.root = reinterpret_cast<NodeRef>(push_leaf_node(this->header));
        this->header.leaves = this->header.root;
    }

    static void deallocate_tree(BTreeInMemory& btree, Node* node, CountType depth) {
        if (depth == btree.header.depth) {
            os::deallocate(node);
        } else {
            for (const auto& child_ref : children(node, btree.header))
                deallocate_tree(btree, reinterpret_cast<Node*>(child_ref), depth + 1);
            os::deallocate(node);
        }
    }

    BTreeInMemory::~BTreeInMemory() {
        deallocate_tree(*this, reinterpret_cast<Node*>(this->header.root), 0);
    }

    BTreeInMemory::Transaction::Transaction(): t(nullptr) {}
    BTreeInMemory::Transaction::Transaction(BTreeInMemory* t): t(t) {}
    BTreeInMemory::Transaction::Transaction(Transaction&& other): t(other.t)  {}
    BTreeInMemory::Transaction scope(const BTreeInMemory::Transaction& t) {
        return BTreeInMemory::Transaction(t.t);
    }
}