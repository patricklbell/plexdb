#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <iostream>
#include <algorithm>
#include <vector>
#include <numeric>

import plexdb.base;
import plexdb.os;
import plexdb.btree;
import plexdb.btree.detail;

namespace plexdb::btree {
    template<typename T>
    std::string node_to_string(Node* node, const BTreeSettings& s, bool is_leaf) {
        std::string result = "[";
        for (int i = 0; i < node->key_count; i++) {
            result += std::to_string(keys(node)[i]);

            if (is_leaf) {
                T value = os::memory_cast<T>(values(node, s)[i]);
                result += ": " + std::to_string(value);
            }

            result += ((i == node->key_count-1) ? "" : ", ");
        }
        return result + "]";
    }

    template<typename T>
    std::string btree_to_string_recursive(BTreeInMemory& t, Node* node, int depth, std::string prepend, bool end) {
        std::string result = prepend + (end ? " └" : " ├");

        result += node_to_string<T>(node, t.settings, depth == t.depth) + "\n";
        if (depth < t.depth) {
            auto chldrn = children(node, t.settings);
            for (auto& child : chldrn) {
                result += btree_to_string_recursive<T>(t, child, depth+1, prepend + (end ? "   " : " | "), &child == chldrn.end()-1);
            }
        }
        return result;
    }
    
    template<typename T>
    std::string btree_to_string(BTreeInMemory& t) {
        std::string result = "";
        result += "[tree, depth=" + std::to_string(t.depth) + "]\n";
        result += btree_to_string_recursive<T>(t, t.root, 0, "", true);
        return result;
    }
}

using namespace plexdb;
using namespace plexdb::btree;

TEST_CASE("insert", "[btree]" ) {
    SECTION("(internal=3,leaf=4) consecutive insertion") {
        BTreeInMemory t(3, 4, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));

            for (int i = 0; i <= key; i++) {
                REQUIRE(get<int>(t, i) == 10*i);
            }
        }
    }

    SECTION("(internal=7,leaf=5) consecutive insertion") {
        BTreeInMemory t(7, 5, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));

            for (int i = 0; i <= key; i++) {
                REQUIRE(get<int>(t, i) == 10*i);
            }
        }
    }


    SECTION("consecutive insertion with combinations of tree settings") {
        for (int max_leaf = 3; max_leaf <= 5; max_leaf++) {
            for (int max_internal = 3; max_internal <= 5; max_internal++) {
                BTreeInMemory t(max_internal, max_leaf, sizeof(int));
        
                for (int key = 0; key < max_leaf*max_internal+1; key++) {    
                    int value = 10*key;
                    insert(t, key, reinterpret_cast<U8*>(&value));
    
                    for (int i = 0; i <= key; i++) {
                        REQUIRE(get<int>(t, i) == 10*i);
                    }
                }
            }
        }
    }

    SECTION("(internal=5,leaf=3) duplicate key insertion overwrites value") {
        BTreeInMemory t(5, 3, sizeof(int));

        for (int key = 0; key < 5; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        for (int key = 0; key < 5; key++) {
            int new_value = 100 * key;
            insert(t, key, reinterpret_cast<U8*>(&new_value));
        }

        for (int key = 0; key < 5; key++) {
            REQUIRE(get<int>(t, key) == 100 * key);
        }
    }

    SECTION("(internal=4,leaf=4) reverse insertion order") {
        BTreeInMemory t(4, 4, sizeof(int));

        for (int key = 31; key >= 0; key--) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        INFO(btree_to_string<int>(t));

        for (int key = 0; key < 32; key++) {
            REQUIRE(get<int>(t, key) == 10*key);
        }
    }

    SECTION("(internal=3,leaf=3) different value sizes and inhomogeneous inserts") {
        struct {
            int key;
            std::vector<U8> value;
        } entries[] = {
            {0, {1, 2, 3, 4}},
            {1, {9}},
            {2, {5, 6, 7, 8, 9}},
            {3, {0}},
            {4, {42, 43}},
        };

        BTreeInMemory t(3, 3, 9);

        for (auto& e : entries) {
            insert(t, e.key, e.value.data());
        }

        for (auto& e : entries) {
            auto s = search(t, e.key);
            REQUIRE(s);
            REQUIRE(std::equal(s.value, s.value + e.value.size(), e.value.begin(), e.value.end()));
        }

        for (auto& e : entries) {
            remove(t, e.key);
            for (auto& f : entries) {
                if (f.key != e.key && f.key > e.key) {
                    auto s = search(t, f.key);
                    REQUIRE(s);
                    REQUIRE(std::equal(s.value, s.value + f.value.size(), f.value.begin(), f.value.end()));
                }
            }
        }
    }

    BENCHMARK_ADVANCED("(internal=5,leaf=7) key with same value")(Catch::Benchmark::Chronometer meter) {
        BTreeInMemory t(5, 7, sizeof(int));

        meter.measure([&t] (int i) {
            insert(t, i, reinterpret_cast<U8*>(&i));
        });
    };
}

TEST_CASE("remove", "[btree]" ) {
    SECTION("(internal=3,leaf=4) consecutive deletion in order") {
        BTreeInMemory t(3, 4, sizeof(int));

        for (int key = 0; key < 32; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        for (int key = 0; key < 32; key++) {
            remove(t, key);
            for (int i = key+1; i < 32; i++) {
                REQUIRE(get<int>(t, i) == 10*i);
            }
        }
    }

    SECTION("(internal=4,leaf=3) reverse deletion order") {
        BTreeInMemory t(4, 3, sizeof(int));

        for (int key = 0; key < 16; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        for (int key = 15; key >= 0; key--) {
            remove(t, key);
            for (int i = 0; i < key; i++) {
                REQUIRE(get<int>(t, i) == 10*i);
            }
        }
    }

    SECTION("consecutive deletion in order with combinations of tree settings") {
        for (int max_leaf = 3; max_leaf <= 5; max_leaf++) {
            for (int max_internal = 3; max_internal <= 5; max_internal++) {
                BTreeInMemory t(max_internal, max_leaf, sizeof(int));
        
                for (int key = 0; key < max_leaf*max_internal*max_internal; key++) {
                    int value = 10*key;
                    insert(t, key, reinterpret_cast<U8*>(&value));
                }

                for (int key = 0; key < max_leaf*max_internal*max_internal; key++) {
                    remove(t, key);
                    for (int i = key+1; i < max_leaf*max_internal*max_internal; i++) {
                        REQUIRE(get<int>(t, i) == 10*i);
                    }
                }
            }
        }
    }

    SECTION("(internal=3,leaf=3) consecutive insertion, deletion permutations") {
        BTreeInMemory t(3, 3, sizeof(int));

        int kept = 3;
        for (int key = 0; key < kept; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        for (int max_key = kept; max_key <= 9; max_key++) {
            std::vector<int> elements(max_key-(kept-1));
            std::iota(elements.begin(), elements.end(), kept);
            
            do {
                // insert in consecutive order
                for (int key = kept; key <= max_key; key++) {
                    int value = 10*key;
                    insert(t, key, reinterpret_cast<U8*>(&value));
                }
                
                // remove in permutation order
                for (int i = 0; i < elements.size(); i++) {
                    remove(t, elements[i]);
                    for (int j = i+1; j < elements.size(); j++) {
                        REQUIRE(get<int>(t, elements[j]) == 10*elements[j]);
                    }
                }

                CAPTURE(max_key);
                REQUIRE(size(t) == kept);
            } while (std::next_permutation(elements.begin(), elements.end()));
        }
    }

    SECTION("(internal=4, leaf=4) reinsertion after complete deletion") {
        BTreeInMemory t(4, 4, sizeof(int));

        for (int key = 0; key < 8; key++) {
            int value = 10*key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        for (int key = 0; key < 8; key++) {
            remove(t, key);
        }

        for (int key = 0; key < 8; key++) {
            int value = 100 * key;
            insert(t, key, reinterpret_cast<U8*>(&value));
        }

        for (int key = 0; key < 8; key++) {
            REQUIRE(get<int>(t, key) == 100 * key);
        }
    }
}