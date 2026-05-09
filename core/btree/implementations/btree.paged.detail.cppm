export module plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.btree.paged;

export namespace plexdb::btree {
    using TransactionPaged = BTreePaged::Transaction;
    coroutine::Task<const Header*> read_header(TransactionPaged& transaction);
    coroutine::Task<Header*>       update_header(TransactionPaged& transaction);
    coroutine::Task<const Node*>   read_node(TransactionPaged& transaction, const NodeRef& ref);
    coroutine::Task<Node*>         update_node(TransactionPaged& transaction, const NodeRef& ref);

    coroutine::Task<NodeRef>       create_internal(TransactionPaged& transaction);
    coroutine::Task<NodeRef>       create_leaf(TransactionPaged& transaction);
    coroutine::Task<>              delete_node(TransactionPaged& transaction, const NodeRef& ref);
}
