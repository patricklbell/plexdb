export module plexdb.btree.detail;

import plexdb.base;

export namespace plexdb::btree {
    using KeyType = U64;
    using CountType = U16;

    struct Node {
        CountType key_count;
        Node *prev, *next;
    };

    struct BTreeSettings {
        CountType max_keys_per_internal;
        CountType max_keys_per_leaf;
        U64 value_stride;
    };

    KeyType* keys_raw(Node* node);
    Node** children_raw(Node* internal, const BTreeSettings& s);
    U8* values_raw(Node* leaf, const BTreeSettings& s);

    TArrayView<KeyType, CountType> keys(Node* node);
    TArrayView<Node*, CountType> children(Node* internal, const BTreeSettings& s);
    ArrayView<U64, CountType> values(Node* leaf, const BTreeSettings& s);

    Node* make_internal(const BTreeSettings& s);
    Node* make_leaf(const BTreeSettings& s);

    struct Search {
        Node* leaf;
        CountType idx;
        U8* value;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return leaf != nullptr; }
    };
}