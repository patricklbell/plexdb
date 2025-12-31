export module plexdb.btree.print;

import plexdb.base;
import plexdb.os;
import plexdb.btree;
import plexdb.btree.detail;

namespace plexdb::btree {
    template<typename T>
    AutoString8 node_to_string(const Node* node, const Header& h, bool is_leaf) {
        AutoString8 res = "[";
        for (int i = 0; i < node->key_count; i++) {
            res += to_string8(keys(node)[i]);

            if (is_leaf) {
                T value = os::memory_cast<T>(values(node, h)[i]);
                res += ": " + to_string8(value);
            }

            res += ((i == node->key_count-1) ? "" : ", ");
        }
        return res + "]";
    }

    template<typename T, typename Transaction>
    AutoString8 btree_to_string_recursive(Transaction& t, const Node* node, int depth, const AutoString8& prepend, bool end) {
        const auto& h = *rheader(t);

        AutoString8 res = prepend + (end ? " └" : " ├");
        res += node_to_string<T>(node, h, depth == h.depth) + "\n";
        if (depth < h.depth) {
            auto cs = children(node, h);
            for (auto& child_ref : cs) {
                const Node* child = rnode(t, child_ref);
                res += btree_to_string_recursive<T>(t, child, depth+1, prepend + (end ? "   " : " | "), &child_ref == cs.end()-1);
            }
        }
        return res;
    }

    export namespace print {
        template<typename T, typename BTree>
        AutoString8 to_string8(BTree& btree) {
            using Transaction = typename BTree::Transaction;
            Transaction t{&btree};

            AutoString8 res = "";
            res += "[tree, depth=" + plexdb::to_string8(rheader(t)->depth) + "]\n";
            res += btree_to_string_recursive<T>(t, rnode(t, rheader(t)->root), 0, "", true);
            return res;
        }
    }
}