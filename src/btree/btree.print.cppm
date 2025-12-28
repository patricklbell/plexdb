export module plexdb.btree.print;

import plexdb.base;
import plexdb.os;
import plexdb.btree;
import plexdb.btree.detail;

namespace plexdb::btree {
    template<typename T>
    AutoString8 node_to_string(Node* node, const Settings& s, bool is_leaf) {
        AutoString8 res = "[";
        for (int i = 0; i < node->key_count; i++) {
            res += to_string8(keys(node)[i]);

            if (is_leaf) {
                T value = os::memory_cast<T>(values(node, s)[i]);
                res += ": " + to_string8(value);
            }

            res += ((i == node->key_count-1) ? "" : ", ");
        }
        return res + "]";
    }

    template<typename T, typename BTree>
    AutoString8 btree_to_string_recursive(BTree& t, Node* node, int depth, const AutoString8& prepend, bool end) {
        const auto& s = get_settings(t);

        AutoString8 res = prepend + (end ? " └" : " ├");
        res += node_to_string<T>(node, s, depth == s.depth) + "\n";
        if (depth < s.depth) {
            auto chldrn = children(node, s);
            for (auto& child_ref : chldrn) {
                Node* child = rnode(t, child_ref);
                res += btree_to_string_recursive<T>(t, child, depth+1, prepend + (end ? "   " : " | "), &child_ref == chldrn.end()-1);
            }
        }
        return res;
    }

    export namespace print {
        template<typename T, typename BTree>
            requires Either<BTree, BTreeInMemory, BTreePaged>
        AutoString8 to_string8(BTree& t) {
            AutoString8 res = "";
            res += "[tree, depth=" + plexdb::to_string8(get_settings(t).depth) + "]\n";
            res += btree_to_string_recursive<T>(t, rnode(t, get_root(t)), 0, "", true);
            return res;
        }
    }
}