module plexdb.btree.detail;

import plexdb.os;

namespace plexdb::btree {
    KeyType* keys_raw(Node* node) { return reinterpret_cast<KeyType*>(reinterpret_cast<U8*>(node)+sizeof(*node)); }
    Node** children_raw(Node* node, const BTreeSettings& s) { return reinterpret_cast<Node**>(reinterpret_cast<U8*>(node)+sizeof(*node)+s.max_keys_per_internal*sizeof(KeyType)); }
    U8* values_raw(Node* node, const BTreeSettings& s) { return reinterpret_cast<U8*>(node)+sizeof(*node)+s.max_keys_per_leaf*sizeof(KeyType); }

    TArrayView<KeyType, CountType> keys(Node* node) { return TArrayView{keys_raw(node), node->key_count}; }
    TArrayView<Node*, CountType> children(Node* node, const BTreeSettings& s) { return TArrayView{children_raw(node, s), static_cast<CountType>(node->key_count+1)}; }
    ArrayView<U64, CountType> values(Node* node, const BTreeSettings& s) { return ArrayView{values_raw(node, s), s.value_stride, node->key_count}; }

    Node* make_internal(const BTreeSettings& s) {
        Node* node = reinterpret_cast<Node*>(os::allocate(
            sizeof(*node) + 
            s.max_keys_per_internal*sizeof(KeyType) + 
            (s.max_keys_per_internal+1)*sizeof(Node*)
        ));
        node->key_count = 0;
        node->prev = nullptr;
        node->next = nullptr;
        return node;
    }

    Node* make_leaf(const BTreeSettings& s) {
        Node* node = reinterpret_cast<Node*>(os::allocate(
            sizeof(*node) + 
            s.max_keys_per_leaf*sizeof(KeyType) + 
            s.max_keys_per_leaf*s.value_stride
        ));
        node->key_count = 0;
        node->prev = nullptr;
        node->next = nullptr;
        return node;
    }
}