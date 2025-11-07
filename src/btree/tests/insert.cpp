import plexdb.btree;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("insert", "[btree]" ) {
    using namespace plexdb;
    using namespace plexdb::btree;

    BTreeInMemory t(3, 3, sizeof(int));

    SECTION("repeated serial insertion") {
        int key = 0, value_wrt = 10;
        insert(t, key, reinterpret_cast<U8*>(&value_wrt)); key++; value_wrt++;
        insert(t, key, reinterpret_cast<U8*>(&value_wrt)); key++; value_wrt++;
        insert(t, key, reinterpret_cast<U8*>(&value_wrt)); key++; value_wrt++;
        insert(t, key, reinterpret_cast<U8*>(&value_wrt)); key++; value_wrt++;
        insert(t, key, reinterpret_cast<U8*>(&value_wrt)); key++; value_wrt++;

        REQUIRE(*reinterpret_cast<int*>(search(t, 0).value) == 10);
        REQUIRE(*reinterpret_cast<int*>(search(t, 1).value) == 11);
    }
}