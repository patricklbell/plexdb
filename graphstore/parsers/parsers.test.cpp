#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.tagged_union;
import graphstore.parsers;
import graphstore.engine.statements;

using namespace plexdb;
using namespace graphstore;
using namespace graphstore::parsers;

TEST_CASE("SPARQL SELECT parse returns empty for stub", "[graphstore.parser]") {
    auto result = sparql::parse("SELECT ?name WHERE { ?s <http://example.org/name> ?name }");
    REQUIRE_FALSE(static_cast<bool>(result));
}

TEST_CASE("SPARQL INSERT DATA parse returns empty for stub", "[graphstore.parser]") {
    auto result = sparql::parse("INSERT DATA { <http://example.org/s> <http://example.org/p> \"value\" }");
    REQUIRE_FALSE(static_cast<bool>(result));
}
