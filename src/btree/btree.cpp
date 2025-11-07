module plexdb.btree;

import plexdb.os;

namespace plexdb::btree {
    // @todo assert key counts
    BTreeSettings::BTreeSettings(CountType keys_per_internal_node, CountType keys_per_leaf_node, U64 value_stride):
        keys_per_internal_node(keys_per_internal_node), keys_per_leaf_node(keys_per_leaf_node), value_stride(value_stride) {}

    BTreeInMemory::BTreeInMemory(CountType keys_per_internal_node, CountType keys_per_leaf_node, U64 value_stride):
        settings(keys_per_internal_node, keys_per_leaf_node, value_stride) {
        leaves = make_leaf(settings);
    }

    BTreeInMemory::~BTreeInMemory() {
        // @todo
    }

    // 
    // helpers
    // 
    InternalNode* make_internal(const BTreeSettings& settings) {
        InternalNode* result;
        U8* block = os::allocate(
            sizeof(*result) + 
            settings.keys_per_internal_node*sizeof(*result->keys) + 
            (settings.keys_per_internal_node+1)*sizeof(*result->children)
        );

        result = reinterpret_cast<InternalNode*>(block);
        result->keys = reinterpret_cast<KeyType*>(
            result + 
            sizeof(*result)
        );
        result->children = reinterpret_cast<InternalNodeChildPtr*>(
            result + 
            sizeof(*result) + 
            settings.keys_per_internal_node*sizeof(*result->keys)
        );
        return result;
    }

    LeafNode* make_leaf(const BTreeSettings& settings) {
        LeafNode* result;
        U8* block = os::allocate(
            sizeof(*result) + 
            settings.keys_per_leaf_node*(sizeof(*result->keys) + settings.value_stride)
        );

        result = reinterpret_cast<LeafNode*>(block);
        result->keys = reinterpret_cast<KeyType*>(
            block + 
            sizeof(*result)
        );
        result->values = block + 
                         sizeof(*result) + 
                         settings.keys_per_leaf_node*sizeof(*result->keys);
        return result;
    }

    void shift_internal_children(InternalNode* parent, CountType child_idx) {
        for (CountType j = parent->key_count; j > child_idx; j++)
            parent->children[j+1] = parent->children[j];
        for (CountType j = parent->key_count-1; j >= child_idx; j++)
            parent->keys[j+1] = parent->keys[j];
        parent->key_count++;
    }

    // @precondition child is full (2t-1 keys)
    // @precondition parent is not full or null
    void split_child_internal(InternalNode* parent, CountType child_idx, const BTreeSettings& settings) {
        // copy right half of child's keys+children to new sibling
        InternalNode* full_child = parent->children[child_idx].internal;
        CountType t = (full_child->key_count+1)/2;

        InternalNode* new_child = make_internal(settings);
        new_child->key_count = t-1;
        memcpy(new_child->keys, full_child->keys+t, (t-1)*sizeof(*new_child->keys));
        memcpy(new_child->children, full_child->children+t, t*sizeof(*new_child->children));
        full_child->key_count = t-1;

        // shift siblings down and insert new child
        shift_internal_children(parent, child_idx);
        parent->children[child_idx+1].internal = new_child;
        parent->keys[child_idx] = full_child->keys[t+1];
    }

    void split_child_leaf(InternalNode* parent, CountType child_idx, const BTreeSettings& settings) {
        // copy right half of child's keys+children to new sibling
        LeafNode* full_child = parent->children[child_idx].leaf;
        CountType t = full_child->key_count/2;

        LeafNode* new_child = make_leaf(settings);
        new_child->key_count = t;
        CountType offset = full_child->key_count-t;
        memcpy(new_child->keys, full_child->keys+offset, t*sizeof(*new_child->keys));
        memcpy(new_child->values, full_child->values+offset*settings.value_stride, t*settings.value_stride);
        full_child->key_count -= t;

        // shift siblings down and insert new child
        shift_internal_children(parent, child_idx);
        parent->children[child_idx+1].leaf = new_child;
        parent->keys[child_idx] = full_child->keys[offset+1];

        // insert into doubly linked list
        new_child->next = full_child->next;
        new_child->prev = full_child;
        full_child->next = new_child;
    }

    // @note returns first key index >= key
    CountType search_keys(const KeyType* keys, CountType count, KeyType key) {
        CountType L = 0, R = count;
        while (L < R) {
            CountType M = L + (R - L)/2;

            if (keys[M] < key) {
                L = M+1;
            } else {
                R = M;
            }
        }

        return L;
    }

    U8* insert_view_leaf_nonfull(LeafNode* leaf, KeyType key, const BTreeSettings& settings) {
        auto& s = settings.value_stride;

        CountType j = 0;
        if (leaf->key_count > 0) {
            j = leaf->key_count-1;
            for (; j >= 0 && key < leaf->keys[j]; j--)
                leaf->keys[j+1] = leaf->keys[j];    
            j++;
            memmove(leaf->values + (j+1)*s, leaf->values + j*s, (leaf->key_count-j)*s);
        }

        leaf->keys[j] = key;
        leaf->key_count++;
        return leaf->values+j*s;
    }

    U8* insert_view_nonfull(BTreeInMemory& btree, void* node, CountType depth, KeyType key) {
        if (depth == btree.depth) {
            return insert_view_leaf_nonfull(reinterpret_cast<LeafNode*>(node), key, btree.settings);
        } else {
            InternalNode* n = reinterpret_cast<InternalNode*>(node);
            CountType child_idx = search_keys(n->keys, n->key_count, key);
            child_idx++;

            if (depth + 1 == btree.depth) {
                LeafNode* c = n->children[child_idx].leaf;
                if (c->key_count == btree.settings.keys_per_leaf_node) {
                    split_child_leaf(n, child_idx, btree.settings);

                    // which child to recurse into
                    child_idx = (key > n->keys[child_idx]) ? (child_idx+1) : child_idx;
                }
            } else {
                InternalNode* c = n->children[child_idx].internal;
                if (c->key_count == btree.settings.keys_per_internal_node) {
                    split_child_internal(n, child_idx, btree.settings);

                    // which child to recurse into
                    child_idx = (key > n->keys[child_idx]) ? (child_idx+1) : child_idx;
                }
            }

            return insert_view_nonfull(btree, n->children[child_idx].any, depth+1, key);
        }
    }

    Search search_recursive(const BTreeInMemory& btree, KeyType key) {
        const LeafNode* leaf = btree.leaves;

        if (leaf == nullptr)
            return Search{};
        if (btree.depth > 0) {
            const InternalNode* curr = btree.root;
            for (CountType depth = 0; depth <= btree.depth; depth++) {
                CountType idx = search_keys(curr->keys, curr->key_count, key);
                curr = curr->children[idx].internal;
            }
            leaf = reinterpret_cast<const LeafNode*>(curr);
        }
        
        CountType idx = search_keys(leaf->keys, leaf->key_count, key);

        if (leaf->keys[idx] == key) {
            return Search{leaf, idx, leaf->values+idx*btree.settings.value_stride};
        }
        return Search{};
    }

    // 
    // api
    // 
    U8* insert_view(BTreeInMemory& btree, KeyType key) {
        void* root = reinterpret_cast<void*>(btree.root);

        if (btree.depth == 0) {
            root = reinterpret_cast<void*>(btree.leaves);

            // split root leaf if needed
            if (btree.leaves->key_count == btree.settings.keys_per_leaf_node) {
                btree.root = make_internal(btree.settings);
                btree.root->children[0].leaf = btree.leaves;
                split_child_leaf(btree.root, 0, btree.settings);

                btree.depth++;
                root = reinterpret_cast<void*>(btree.root);
            }
        }
        
        return insert_view_nonfull(btree, root, 0, key);
    }

    void insert(BTreeInMemory& btree, KeyType key, U8* value) {
        U8* view = insert_view(btree, key);
        memcpy(view, value, btree.settings.value_stride);
    }

    Search search(const BTreeInMemory& btree, KeyType key) {
        return search_recursive(btree, key);
    }
}