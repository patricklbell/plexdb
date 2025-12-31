export module plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.btree.types;
import plexdb.btree.paged;

export namespace plexdb::btree {
    using Transaction = BTreePaged::Transaction;
    const Header* rheader(Transaction& transaction);
    Header*       rwheader(Transaction& transaction);
    const Node*   rnode(Transaction& transaction, const NodeRef& ref);
    Node*         rwnode(Transaction& transaction, const NodeRef& ref);

    NodeRef       new_internal(Transaction& transaction);
    NodeRef       new_leaf(Transaction& transaction);
    void          delete_node(Transaction& transaction, const NodeRef& ref);
}