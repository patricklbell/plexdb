export module plexdb.btree.in_memory.detail;

import plexdb.base;
import plexdb.btree.types;
import plexdb.btree.node;
import plexdb.btree.in_memory;

export namespace plexdb::btree {
    using TransactionInMemory = BTreeInMemory::Transaction;
    const Header* read_header(TransactionInMemory& transaction);
    Header*       update_header(TransactionInMemory& transaction);
    const Node*   read_node(TransactionInMemory& transaction, const NodeRef& ref);
    Node*         update_node(TransactionInMemory& transaction, const NodeRef& ref);

    NodeRef       create_internal(TransactionInMemory& transaction);
    NodeRef       create_leaf(TransactionInMemory& transaction);
    void          delete_node(TransactionInMemory& transaction, const NodeRef& ref);
}