module plexdb.btree.node;

import plexdb.os;

namespace plexdb::btree {
    KeyType* keys_raw(Node* node)                        { return reinterpret_cast<KeyType*>(reinterpret_cast<U8*>(node)+sizeof(*node)); }
    NodeRef* children_raw(Node* node, const Settings& s) { return reinterpret_cast<NodeRef*>(reinterpret_cast<U8*>(node)+sizeof(*node)+s.max_keys_per_internal*sizeof(KeyType)); }
    U8*      values_raw(Node* node, const Settings& s)   { return reinterpret_cast<U8*>(node)+sizeof(*node)+s.max_keys_per_leaf*sizeof(KeyType); }

    TArrayView<KeyType, CountType>  keys(Node* node)                        { return TArrayView{keys_raw(node), node->key_count}; }
    TArrayView<NodeRef, CountType>  children(Node* node, const Settings& s) { return TArrayView{children_raw(node, s), static_cast<CountType>(node->key_count+1)}; }
    ArrayView<CountType>            values(Node* node, const Settings& s)   { return ArrayView{values_raw(node, s), s.value_stride, node->key_count}; }

    Node* push_internal_node(const Settings& s) {
        U64 node_size = sizeof(Node) + s.max_keys_per_internal*sizeof(KeyType) + (s.max_keys_per_internal+1)*sizeof(Node*);
        Node* node = reinterpret_cast<Node*>(os::allocate(node_size));
        node->key_count = 0;
        node->prev = ~0u;
        node->next = ~0u;
        return node;
    }

    Node* push_leaf_node(const Settings& s) {
        U64 node_size = sizeof(Node) + s.max_keys_per_leaf*sizeof(KeyType) + s.max_keys_per_leaf*s.value_stride;
        Node* node = reinterpret_cast<Node*>(os::allocate(node_size));
        node->key_count = 0;
        node->prev = ~0u;
        node->next = ~0u;
        return node;
    }
}