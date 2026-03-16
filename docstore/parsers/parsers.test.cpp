#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import docstore.parsers;
import docstore.engine.statements;

using namespace plexdb;
using namespace docstore;

TEST_CASE("docstore statement types", "[docstore.parser]") {
    SECTION("find documents with filter") {
        FindDocuments find{};
        find.database_name = "testdb";
        find.collection_name = "users";
        find.limit = 10;
        push_back(find.filter, FilterExpr{.field = "age", .op = FilterOp::gt, .value = AutoString8("18")});

        Statement stmt{};
        stmt.value = move(find);
        REQUIRE(type_matches_tag<FindDocuments>(stmt.value));
        REQUIRE(get<FindDocuments>(stmt.value).limit == 10);
        REQUIRE(get<FindDocuments>(stmt.value).filter.cap == 1);
    }

    SECTION("insert document") {
        InsertDocument insert{};
        insert.database_name = "testdb";
        insert.collection_name = "users";
        push_back(insert.documents, AutoString8("{\"name\": \"alice\"}"));

        Statement stmt{};
        stmt.value = move(insert);
        REQUIRE(type_matches_tag<InsertDocument>(stmt.value));
        REQUIRE(get<InsertDocument>(stmt.value).documents.length == 1);
    }
}
