export module plexdb.btree.in_memory.detail;

import plexdb.base;
import plexdb.btree.types;
import plexdb.btree.node;
import plexdb.btree.in_memory;

export namespace plexdb::btree {
    using Transaction = BTreeInMemory::Transaction;
    const Header* rheader(Transaction& transaction);
    Header*       rwheader(Transaction& transaction);
    const Node*   rnode(Transaction& transaction, const NodeRef& ref);
    Node*         rwnode(Transaction& transaction, const NodeRef& ref);

    NodeRef       new_internal(Transaction& transaction);
    NodeRef       new_leaf(Transaction& transaction);
    void          delete_node(Transaction& transaction, const NodeRef& ref);
}