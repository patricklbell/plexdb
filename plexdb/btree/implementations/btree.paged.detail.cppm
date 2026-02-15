export module plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.btree.types;
import plexdb.btree.paged;

export namespace plexdb::btree {
    using TransactionPaged = BTreePaged::Transaction;
    const Header* read_header(TransactionPaged& transaction);
    Header*       update_header(TransactionPaged& transaction);
    const Node*   read_node(TransactionPaged& transaction, const NodeRef& ref);
    Node*         update_node(TransactionPaged& transaction, const NodeRef& ref);

    NodeRef       create_internal(TransactionPaged& transaction);
    NodeRef       create_leaf(TransactionPaged& transaction);
    void          delete_node(TransactionPaged& transaction, const NodeRef& ref);
}