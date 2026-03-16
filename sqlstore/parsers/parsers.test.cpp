#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import sqlstore.parsers;
import sqlstore.engine.statements;

using namespace plexdb;
using namespace sqlstore;

TEST_CASE("sqlstore statement types", "[sqlstore.parser]") {
    SECTION("select with join and spatial filter") {
        Select sel{};
        sel.schema_name = "public";
        sel.table_name = "places";
        sel.limit = 100;
        push_back(sel.columns, String8("name"));
        push_back(sel.columns, String8("geom"));
        push_back(sel.joins, JoinClause{
            .type = JoinType::inner,
            .table = TableRef{.schema_name = "public", .table_name = "categories"},
            .left_column = "category_id",
            .right_column = "id",
        });

        Statement stmt{};
        stmt.value = move(sel);
        REQUIRE(type_matches_tag<Select>(stmt.value));
        REQUIRE(get<Select>(stmt.value).joins.cap == 1);
        REQUIRE(get<Select>(stmt.value).columns.cap == 2);
    }

    SECTION("create table with spatial column") {
        CreateTable ct{};
        ct.schema_name = "public";
        ct.table_name = "places";
        push_back(ct.columns, ColumnDef{.name = "id", .type = DataType::integer, .primary_key = true});
        push_back(ct.columns, ColumnDef{.name = "geom", .type = DataType::point, .spatial_index = true});

        Statement stmt{};
        stmt.value = move(ct);
        REQUIRE(type_matches_tag<CreateTable>(stmt.value));
        REQUIRE(get<CreateTable>(stmt.value).columns.length == 2);
    }
}
