#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.tagged_union;
import docstore.parsers;
import docstore.engine.statements;

using namespace plexdb;
using namespace docstore;
using namespace docstore::parsers;

TEST_CASE("MQL parse returns empty for stub", "[docstore.parser]") {
    auto result = mql::parse("db.collection.find({})");
    REQUIRE_FALSE(static_cast<bool>(result));
}
