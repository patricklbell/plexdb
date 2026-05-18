export module plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.pager;
import plexdb.btree.paged;

export namespace plexdb::btree {
    template<KeyPolicy KP, ValuePolicy VP>
    using TransactionPaged = typename BTreePaged<KP,VP>::Transaction;

    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Header*> read_header(TransactionPaged<KP,VP>& t) {
        co_return reinterpret_cast<const Header*>(co_await pager::rpage(*t.t->pager, t.t->header_page));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Header*> update_header(TransactionPaged<KP,VP>& t) {
        co_return reinterpret_cast<Header*>(co_await pager::rwpage(*t.t->pager, t.t->header_page));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<const Node*> read_node(TransactionPaged<KP,VP>& t, const NodeRef& ref) {
        co_return reinterpret_cast<const Node*>(co_await pager::rpage(*t.t->pager, ref));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<Node*> update_node(TransactionPaged<KP,VP>& t, const NodeRef& ref) {
        co_return reinterpret_cast<Node*>(co_await pager::rwpage(*t.t->pager, ref));
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_internal(TransactionPaged<KP,VP>& t) {
        co_return co_await pager::new_page(*t.t->pager);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<NodeRef> create_leaf(TransactionPaged<KP,VP>& t) {
        co_return co_await pager::new_page(*t.t->pager);
    }
    template<KeyPolicy KP, ValuePolicy VP>
    coroutine::Task<> delete_node(TransactionPaged<KP,VP>& t, const NodeRef& ref) {
        co_await pager::delete_page(*t.t->pager, ref);
    }
}
