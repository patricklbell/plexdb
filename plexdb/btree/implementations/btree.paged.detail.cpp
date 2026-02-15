module plexdb.btree.paged.detail;

import plexdb.pager;

namespace plexdb::btree {
    using TransactionPaged = BTreePaged::Transaction;
    const Header* read_header(TransactionPaged& t)                      { return reinterpret_cast<const Header*>(pager::rpage(*t.t->pager, t.t->header_page)); }
    Header*       update_header(TransactionPaged& t)                    { return reinterpret_cast<Header*>(pager::rwpage(*t.t->pager, t.t->header_page)); }
    const Node*   read_node(TransactionPaged& t, const NodeRef& ref)    { return reinterpret_cast<const Node*>(pager::rpage(*t.t->pager, ref)); }
    Node*         update_node(TransactionPaged& t, const NodeRef& ref)  { return reinterpret_cast<Node*>(pager::rwpage(*t.t->pager, ref)); }
    
    NodeRef       create_internal(TransactionPaged& t)                  { return pager::new_page(*t.t->pager); }
    NodeRef       create_leaf(TransactionPaged& t)                      { return pager::new_page(*t.t->pager); }
    void          delete_node(TransactionPaged& t, const NodeRef& ref)  { pager::delete_page(*t.t->pager, ref); }
}