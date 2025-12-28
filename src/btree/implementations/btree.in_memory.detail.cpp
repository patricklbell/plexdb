module plexdb.btree.in_memory.detail;

import plexdb.os;

namespace plexdb::btree {
    Settings& get_settings(BTreeInMemory& btree) {
        return btree.settings;
    }
    NodeRef get_root(BTreeInMemory& btree) {
        return reinterpret_cast<NodeRef>(btree.root);
    }
    void set_root(BTreeInMemory& btree, const NodeRef& new_root) {
        btree.root = rnode(btree, new_root);
    }
    NodeRef create_internal(BTreeInMemory& btree) {
        return reinterpret_cast<NodeRef>(push_internal_node(btree.settings));
    }
    NodeRef create_leaf(BTreeInMemory& btree) {
        return reinterpret_cast<NodeRef>(push_leaf_node(btree.settings));
    }
    void delete_node(BTreeInMemory& btree, const NodeRef& ref) {
        os::deallocate(reinterpret_cast<void*>(ref));
    }
    Node* rnode(BTreeInMemory& btree, const NodeRef& ref) {
        return reinterpret_cast<Node*>(ref);
    }
    Node* rwnode(BTreeInMemory& btree, const NodeRef& ref) {
        return rnode(btree, ref);
    }
}