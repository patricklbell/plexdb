module;
#include <coroutine>

module plexdb.btree.paged.detail;

import plexdb.pager;
import plexdb.coroutine;

namespace plexdb::btree {
    using TransactionPaged = BTreePaged::Transaction;
    coroutine::Task<const Header*> read_header(TransactionPaged& t)                     { co_return reinterpret_cast<const Header*>(co_await pager::rpage(*t.t->pager, t.t->header_page)); }
    coroutine::Task<Header*>       update_header(TransactionPaged& t)                   { co_return reinterpret_cast<Header*>(co_await pager::rwpage(*t.t->pager, t.t->header_page)); }
    coroutine::Task<const Node*>   read_node(TransactionPaged& t, const NodeRef& ref)   { co_return reinterpret_cast<const Node*>(co_await pager::rpage(*t.t->pager, ref)); }
    coroutine::Task<Node*>         update_node(TransactionPaged& t, const NodeRef& ref) { co_return reinterpret_cast<Node*>(co_await pager::rwpage(*t.t->pager, ref)); }

    coroutine::Task<NodeRef>       create_internal(TransactionPaged& t)                 { co_return co_await pager::new_page(*t.t->pager); }
    coroutine::Task<NodeRef>       create_leaf(TransactionPaged& t)                     { co_return co_await pager::new_page(*t.t->pager); }
    coroutine::Task<>              delete_node(TransactionPaged& t, const NodeRef& ref) { co_await pager::delete_page(*t.t->pager, ref); }
}
