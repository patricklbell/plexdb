module;
#include <coroutine>

module plexdb.btree.in_memory.detail;

import plexdb.os;

namespace plexdb::btree {
    using TransactionInMemory = BTreeInMemory::Transaction;
    coroutine::Task<const Header*> read_header(TransactionInMemory& t)                   { co_return &t.t->header; }
    coroutine::Task<Header*>       update_header(TransactionInMemory& t)                 { co_return &t.t->header; }
    coroutine::Task<const Node*>   read_node(TransactionInMemory&, const NodeRef& ref)   { co_return reinterpret_cast<const Node*>(ref); }
    coroutine::Task<Node*>         update_node(TransactionInMemory&, const NodeRef& ref) { co_return reinterpret_cast<Node*>(ref); }

    coroutine::Task<NodeRef>       create_internal(TransactionInMemory& t)               { co_return reinterpret_cast<NodeRef>(push_internal_node(t.t->header)); }
    coroutine::Task<NodeRef>       create_leaf(TransactionInMemory& t)                   { co_return reinterpret_cast<NodeRef>(push_leaf_node(t.t->header)); }
    coroutine::Task<>              delete_node(TransactionInMemory&, const NodeRef& ref) { os::deallocate(reinterpret_cast<void*>(ref)); co_return; }
}
