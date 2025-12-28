export module plexdb.btree.paged.detail;

import plexdb.base;
import plexdb.os;
import plexdb.btree.types;
import plexdb.btree.paged;

export namespace plexdb::btree {
    Settings& get_settings(BTreePaged& btree);
    NodeRef   get_root(BTreePaged& btree);
    void      set_root(BTreePaged& btree, const NodeRef& new_root);
    NodeRef   create_internal(BTreePaged& btree);
    NodeRef   create_leaf(BTreePaged& btree);
    void      delete_node(BTreePaged& btree, const NodeRef& ref);
    Node*     rnode(BTreePaged& btree, const NodeRef& ref);
    Node*     rwnode(BTreePaged& btree, const NodeRef& ref);
}