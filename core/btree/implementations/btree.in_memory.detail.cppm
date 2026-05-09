export module plexdb.btree.in_memory.detail;

import plexdb.base;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.btree.node;
import plexdb.btree.in_memory;

export namespace plexdb::btree {
    using TransactionInMemory = BTreeInMemory::Transaction;
    coroutine::Task<const Header*> read_header(TransactionInMemory& transaction);
    coroutine::Task<Header*>       update_header(TransactionInMemory& transaction);
    coroutine::Task<const Node*>   read_node(TransactionInMemory& transaction, const NodeRef& ref);
    coroutine::Task<Node*>         update_node(TransactionInMemory& transaction, const NodeRef& ref);

    coroutine::Task<NodeRef>       create_internal(TransactionInMemory& transaction);
    coroutine::Task<NodeRef>       create_leaf(TransactionInMemory& transaction);
    coroutine::Task<>              delete_node(TransactionInMemory& transaction, const NodeRef& ref);
}
