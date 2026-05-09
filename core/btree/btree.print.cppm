module;
#include <coroutine>

export module plexdb.btree.print;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.btree;
import plexdb.btree.detail;

using namespace plexdb::btree;

namespace plexdb {
    template<typename T>
    AutoString8 node_to_str(const Node* node, const Header& h, bool is_leaf) {
        AutoString8 res = "["_as;
        for (int i = 0; i < node->key_count; i++) {
            res += to_str(keys(node)[i]);

            if (is_leaf) {
                T value = os::memory_cast<T>(values(node, h)[i]);
                res += ": " + to_str(value);
            }

            res += ((i == node->key_count-1) ? "" : ", ");
        }
        return res + "]";
    }

    template<typename T, typename Transaction>
    AutoString8 btree_to_str_recursive(Transaction& t, const Node* node, U64 depth, const String8& prepend, bool end) {
        auto sync_get = [](auto task) { task.resume(); return move(task.value()); };
        const Header& h = *sync_get(read_header(t));

        AutoString8 res = prepend + String8(end ? " └" : " ├");
        res += node_to_str<T>(node, h, depth == h.depth) + "\n";
        if (depth < h.depth) {
            auto cs = children(node, h);
            for (auto& child_ref : cs) {
                const Node* child = sync_get(read_node(t, child_ref));
                res += btree_to_str_recursive<T>(t, child, depth+1, prepend + (end ? "   " : " | "), &child_ref == cs.end()-1);
            }
        }
        return res;
    }

    export template<typename T, typename BTree>
        requires Either<BTree, BTreeInMemory, BTreePaged>
    AutoString8 to_str(const Tag<T, BTree>& tag) {
        auto sync_get = [](auto task) { task.resume(); return move(task.value()); };
        using Transaction = typename BTree::Transaction;
        Transaction t{tag.value};

        const Header& h = *sync_get(read_header(t));
        AutoString8 res = ""_as;
        res += "[tree, depth=" + to_str(h.depth) + "]\n";
        res += btree_to_str_recursive<T>(t, sync_get(read_node(t, h.root)), 0, "", true);
        return res;
    }
}
