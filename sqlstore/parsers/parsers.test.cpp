#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.tagged_union;
import sqlstore.parsers;
import sqlstore.engine.statements;

using namespace plexdb;
using namespace sqlstore;
using namespace sqlstore::parsers;

TEST_CASE("SQL parse returns empty for stub", "[sqlstore.parser]") {
    auto result = sql::parse("SELECT * FROM users");
    REQUIRE_FALSE(static_cast<bool>(result));
}

TEST_CASE("SQL spatial parse returns empty for stub", "[sqlstore.parser]") {
    auto result = sql::parse("SELECT * FROM places WHERE ST_Contains(geom, ST_Point(1.0, 2.0))");
    REQUIRE_FALSE(static_cast<bool>(result));
}
