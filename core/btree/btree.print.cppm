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

using namespace plexdb::btree;

namespace plexdb {
    template<typename T>
    AutoString8 node_to_str(const Node* node, U32 ns, U16 vs, bool is_leaf) {
        AutoString8 res = "["_as;
        for (int i = 0; i < node->key_count; i++) {
            res += to_str(keys(node)[i]);

            if (is_leaf) {
                T value = os::memory_cast<T>(values(node, ns, vs)[i]);
                res += ": " + to_str(value);
            }

            res += ((i == node->key_count-1) ? "" : ", ");
        }
        return res + "]";
    }

    template<typename T, typename BTree>
    coroutine::Task<AutoString8> btree_to_str_recursive(BTree& t, const Node* node, U64 depth,
                                        const String8& prepend, bool end) {
        const Header& h = *(co_await read_header(t));
        U32 ns = node_size(t);
        U16 vs = static_cast<U16>(h.value_stride);

        AutoString8 res = prepend + String8(end ? " └" : " ├");
        res += node_to_str<T>(node, ns, vs, depth == h.depth) + "\n";
        if (depth < h.depth) {
            auto cs = children(node, ns);
            for (auto& child_ref : cs) {
                const Node* child = co_await read_node(t, child_ref);
                res += co_await btree_to_str_recursive<T>(
                    t, child, depth+1,
                    prepend + (end ? "   " : " | "),
                    &child_ref == cs.end()-1
                );
            }
        }
        co_return res;
    }

    export template<typename T, typename BTree>
        requires Either<BTree, BTreeInMemory<>, BTreePaged<>>
    coroutine::Task<AutoString8> to_str(const Tag<T, BTree>& tag) {
        auto& t = *tag.value;

        const Header& h = *(co_await read_header(t));
        AutoString8 res = ""_as;
        res += "[tree, depth=" + to_str(h.depth) + "]\n";
        res += co_await btree_to_str_recursive<T>(t, co_await read_node(t, h.root), 0, "", true);
        co_return res;
    }
}
