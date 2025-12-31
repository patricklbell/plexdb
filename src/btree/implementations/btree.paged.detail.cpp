module plexdb.btree.paged.detail;

import plexdb.pager;

namespace plexdb::btree {
    using Transaction = BTreePaged::Transaction;
    const Header* rheader(Transaction& t)                   { return reinterpret_cast<const Header*>(pager::rpage(*t.t->pager, t.t->header_page)); }
    Header*       rwheader(Transaction& t)                  { return reinterpret_cast<Header*>(pager::rwpage(*t.t->pager, t.t->header_page)); }
    const Node*   rnode(Transaction& t, const NodeRef& ref) { return reinterpret_cast<const Node*>(pager::rpage(*t.t->pager, ref)); }
    Node*         rwnode(Transaction& t, const NodeRef& ref){ return reinterpret_cast<Node*>(pager::rwpage(*t.t->pager, ref)); }
    
    NodeRef       new_internal(Transaction& t)                   { return pager::new_page(*t.t->pager); }
    NodeRef       new_leaf(Transaction& t)                       { return pager::new_page(*t.t->pager); }
    void          delete_node(Transaction& t, const NodeRef& ref){ pager::delete_page(*t.t->pager, ref); }
}