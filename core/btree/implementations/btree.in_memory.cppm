module;
#include <coroutine>

export module plexdb.btree.in_memory;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

import plexdb.btree.types;
import plexdb.btree.policy;
import plexdb.btree.node;

namespace plexdb::btree {
    export template<KeyPolicy KP, ValuePolicy VP>
    struct BTreeInMemory;

    template<KeyPolicy KP, ValuePolicy VP>
    void deallocate_tree(BTreeInMemory<KP, VP>& btree, Node* node, CountType depth);

    template<KeyPolicy KP, ValuePolicy VP>
    struct BTreeInMemory {
        using kp_type = KP;
        using vp_type = VP;
        Header                   header{};
        SizeType                 node_bytes = 0;
        [[no_unique_address]] KP kp{};
        [[no_unique_address]] VP vp{};

        BTreeInMemory(KP kp, VP vp, SizeType nb)
            : node_bytes(nb)
            , kp(kp)
            , vp(vp) {
            this->header = {
                .depth  = 0,
                .size   = 0,
                .root   = {},
                .leaves = {}
            };

            Node* root          = reinterpret_cast<Node*>(os::allocate(nb));
            root->key_count     = 0;
            root->prev          = 0;
            root->next          = 0;
            this->header.root   = reinterpret_cast<U64>(root);
            this->header.leaves = this->header.root;
        }

        ~BTreeInMemory() {
            deallocate_tree(*this, reinterpret_cast<Node*>(this->header.root), 0);
        }
    };

    template<KeyPolicy KP, ValuePolicy VP>
    void deallocate_tree(BTreeInMemory<KP, VP>& btree, Node* node, CountType depth) {
        if (depth == btree.header.depth) {
            os::deallocate(node);
        } else {
            NodeRef*  ch = internal_children(node, btree.node_bytes, btree.kp);
            CountType n  = static_cast<CountType>(node->key_count + 1);
            for (CountType i = 0; i < n; i++) {
                deallocate_tree(btree, reinterpret_cast<Node*>(ch[i]), static_cast<CountType>(depth + 1));
            }
            os::deallocate(node);
        }
    }
}
