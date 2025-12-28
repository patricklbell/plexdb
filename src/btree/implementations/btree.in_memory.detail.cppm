export module plexdb.btree.in_memory.detail;

import plexdb.base;
import plexdb.btree.types;
import plexdb.btree.node;
import plexdb.btree.in_memory;

export namespace plexdb::btree {
    Settings& get_settings(BTreeInMemory& btree);
    NodeRef   get_root(BTreeInMemory& btree);
    void      set_root(BTreeInMemory& btree, const NodeRef& new_root);
    NodeRef   create_internal(BTreeInMemory& btree);
    NodeRef   create_leaf(BTreeInMemory& btree);
    void      delete_node(BTreeInMemory& btree, const NodeRef& ref);
    Node*     rnode(BTreeInMemory& btree, const NodeRef& ref);
    Node*     rwnode(BTreeInMemory& btree, const NodeRef& ref);
}