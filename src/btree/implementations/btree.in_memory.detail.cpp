module plexdb.btree.in_memory.detail;

import plexdb.os;

namespace plexdb::btree {
    using Transaction = BTreeInMemory::Transaction;
    const Header* rheader(Transaction& t)                     { return &t.t->header; }
    Header*       rwheader(Transaction& t)                    { return &t.t->header; }
    const Node*   rnode(Transaction& t, const NodeRef& ref)   { return reinterpret_cast<Node*>(ref); }
    Node*         rwnode(Transaction& t, const NodeRef& ref)  { return reinterpret_cast<Node*>(ref); }

    NodeRef       new_internal(Transaction& t)                    { return reinterpret_cast<NodeRef>(push_internal_node(t.t->header)); }
    NodeRef       new_leaf(Transaction& t)                        { return reinterpret_cast<NodeRef>(push_leaf_node(t.t->header)); }
    void          delete_node(Transaction& t, const NodeRef& ref) { os::deallocate(reinterpret_cast<void*>(ref)); }
}