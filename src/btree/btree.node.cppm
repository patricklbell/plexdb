export module plexdb.btree.node;

import plexdb.base;
import plexdb.arena;
import plexdb.btree.types;

export namespace plexdb::btree {
    KeyType* keys_raw(Node* node);
    NodeRef* children_raw(Node* internal, const Settings& s);
    U8*      values_raw(Node* leaf, const Settings& s);

    TArrayView<KeyType, CountType> keys(Node* node);
    TArrayView<NodeRef, CountType> children(Node* internal, const Settings& s);
    ArrayView<CountType>           values(Node* leaf, const Settings& s);

    Node* push_internal_node(const Settings& s);
    Node* push_leaf_node(const Settings& s);
}