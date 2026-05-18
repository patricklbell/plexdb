export module plexdb.btree.in_memory.detail;

import plexdb.base;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.btree.node;
import plexdb.btree.policy;
import plexdb.os;
import plexdb.btree.in_memory;

export namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    using TransactionInMemory = typename BTreeInMemory<KP, VP>::Transaction;

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Header*> read_header(TransactionInMemory<KP,VP>& t)   { co_return &t.t->header; }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Header*>       update_header(TransactionInMemory<KP,VP>& t) { co_return &t.t->header; }

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Node*>   read_node(TransactionInMemory<KP,VP>&, const NodeRef& ref) {
        co_return reinterpret_cast<const Node*>(ref);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Node*>         update_node(TransactionInMemory<KP,VP>&, const NodeRef& ref) {
        co_return reinterpret_cast<Node*>(ref);
    }

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_internal(TransactionInMemory<KP,VP>& t) {
        co_return reinterpret_cast<NodeRef>(alloc_internal(t.t->node_bytes));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_leaf(TransactionInMemory<KP,VP>& t) {
        co_return reinterpret_cast<NodeRef>(alloc_leaf(t.t->node_bytes));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> delete_node(TransactionInMemory<KP,VP>&, const NodeRef& ref) {
        os::deallocate(reinterpret_cast<void*>(ref));
        co_return;
    }
}
