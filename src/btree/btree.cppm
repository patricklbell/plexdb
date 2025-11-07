export module plexdb.btree;

export import plexdb.common;

// implements a b+tree
// references:
// - https://bobson.ludost.net/books/algo/book6/chap19.htm
// - https://planetscale.com/blog/btrees-and-database-indexes
namespace plexdb::btree {
    using KeyType = U64;
    using CountType = U16;

    struct InternalNode;
    struct LeafNode;

    union InternalNodeChildPtr {
        InternalNode* internal;
        LeafNode* leaf;
        void* any;
    };
    struct InternalNode {
        CountType key_count;
        KeyType* keys;
        InternalNodeChildPtr* children;
    };

    struct LeafNode {
        CountType key_count;
        KeyType* keys;
        U8* values;

        LeafNode* next;
        LeafNode* prev;
    };

    struct BTreeSettings {
        CountType keys_per_internal_node;
        CountType keys_per_leaf_node;
        U64 value_stride;

        explicit BTreeSettings(CountType keys_per_internal_node, CountType keys_per_leaf_node, U64 value_stride);
    };

    export struct BTreeInMemory {
        InternalNode* root = nullptr;
        LeafNode* leaves = nullptr;
        CountType depth = 0;

        BTreeSettings settings;

        explicit BTreeInMemory(CountType keys_per_internal_node, CountType keys_per_leaf_node, U64 value_stride);
        ~BTreeInMemory();
        BTreeInMemory(const BTreeInMemory& other) = delete;
        BTreeInMemory& operator=(const BTreeInMemory& other) = delete;
    };

    // 
    // helpers
    // 
    InternalNode* make_internal(const BTreeSettings& settings);
    LeafNode* make_leaf(const BTreeSettings& settings);
    void shift_internal_children(InternalNode* parent, CountType child_idx);
    void split_child_internal(InternalNode* parent, CountType child_idx, const BTreeSettings& settings);
    void split_child_leaf(InternalNode* parent, CountType child_idx, const BTreeSettings& settings);
    CountType search_keys(const KeyType* keys, CountType count, KeyType key);
    U8* insert_view_leaf_nonfull(LeafNode* leaf, KeyType key, const BTreeSettings& settings);
    U8* insert_view_nonfull(BTreeInMemory& btree, void* node, CountType depth, KeyType key);
    
    struct Search {
        const LeafNode* node = nullptr;
        CountType idx = 0;
        U8* value = nullptr;
    };
    Search search_recursive(const BTreeInMemory& btree, KeyType key);

    // 
    // api
    // 
    export U8* insert_view(BTreeInMemory& btree, KeyType key);
    export void insert(BTreeInMemory& btree, KeyType key, U8* value);
    export Search search(const BTreeInMemory& btree, KeyType key);
}