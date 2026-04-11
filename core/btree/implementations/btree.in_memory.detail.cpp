module plexdb.btree.in_memory.detail;

import plexdb.os;

namespace plexdb::btree {
    using TransactionInMemory = BTreeInMemory::Transaction;
    const Header* read_header(TransactionInMemory& t)                     { return &t.t->header; }
    Header*       update_header(TransactionInMemory& t)                   { return &t.t->header; }
    const Node*   read_node(TransactionInMemory&, const NodeRef& ref)   { return reinterpret_cast<Node*>(ref); }
    Node*         update_node(TransactionInMemory&, const NodeRef& ref) { return reinterpret_cast<Node*>(ref); }

    NodeRef       create_internal(TransactionInMemory& t)                 { return reinterpret_cast<NodeRef>(push_internal_node(t.t->header)); }
    NodeRef       create_leaf(TransactionInMemory& t)                     { return reinterpret_cast<NodeRef>(push_leaf_node(t.t->header)); }
    void          delete_node(TransactionInMemory&, const NodeRef& ref) { os::deallocate(reinterpret_cast<void*>(ref)); }
}