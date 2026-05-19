module;
#include <coroutine>

export module plexdb.btree.print;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

import plexdb.btree;
import plexdb.btree.detail;
import plexdb.btree.node;
import plexdb.btree.types;
import plexdb.btree.constraint;

using namespace plexdb::btree;

namespace plexdb {
    template<KeyPolicy KP, ValuePolicy VP, typename T>
    AutoString8 node_to_str(const Node* node, U32 ns, KP kp, VP vp, bool is_leaf) {
        AutoString8 res = "["_as;
        for (CountType i = 0; i < node->key_count; i++) {
            if constexpr (KP::is_fixed_size) {
                res += to_str(keys<typename KP::key_type>(node)[i]);
            } else {
                auto kb = is_leaf
                    ? leaf_get_key_bytes(node, ns, kp, vp, i)
                    : internal_get_key_bytes(node, ns, kp, i);
                res += to_str(String8{const_cast<U8*>(kb.ptr), kb.length});
            }

            if (is_leaf) {
                auto vb = leaf_value_const(node, ns, kp, vp, i);
                T value;
                os::memory_copy(&value, vb.ptr, sizeof(T));
                res += ": " + to_str(value);
            }

            res += ((i == node->key_count-1) ? "" : ", ");
        }
        return res + "]";
    }

    template<typename T, BTree BT>
    coroutine::Task<AutoString8> btree_to_str_recursive(BT& t, const Node* node, U64 depth,
                                        const String8& prepend, bool end) {
        const Header& h = *(co_await read_header(t));
        U32 ns = node_size(t);
        auto kp = key_policy(t);
        auto vp = value_policy(t);

        AutoString8 res = prepend + String8(end ? " └" : " ├");
        res += node_to_str<decltype(kp), decltype(vp), T>(node, ns, kp, vp, depth == h.depth) + "\n";
        if (depth < h.depth) {
            const NodeRef* cs = internal_children(node, ns, kp);
            CountType child_count = static_cast<CountType>(node->key_count + 1);
            for (CountType i = 0; i < child_count; i++) {
                const Node* child = co_await read_node(t, cs[i]);
                res += co_await btree_to_str_recursive<T>(
                    t, child, depth+1,
                    prepend + (end ? "   " : " | "),
                    i == child_count - 1
                );
            }
        }
        co_return res;
    }

    export template<typename T, BTree BT>
    coroutine::Task<AutoString8> to_str(const Tag<T, BT>& tag) {
        auto& t = *tag.value;

        const Header& h = *(co_await read_header(t));
        AutoString8 res = ""_as;
        res += "[tree, depth=" + to_str(h.depth) + "]\n";
        res += co_await btree_to_str_recursive<T>(t, co_await read_node(t, h.root), 0, "", true);
        co_return res;
    }
}
