#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import graphstore.parsers;
import graphstore.engine.statements;

using namespace plexdb;
using namespace graphstore;

TEST_CASE("graphstore statement types", "[graphstore.parser]") {
    SECTION("SPARQL select with triple pattern") {
        Select sel{};
        sel.distinct = true;
        sel.limit = 10;
        push_back(sel.variables, String8("name"));
        push_back(sel.where, TriplePattern{
            .subject   = Term{.kind = TermKind::variable, .value = AutoString8("s")},
            .predicate = Term{.kind = TermKind::iri, .value = AutoString8("http://example.org/name")},
            .object    = Term{.kind = TermKind::variable, .value = AutoString8("name")},
        });

        Statement stmt{};
        stmt.value = move(sel);
        REQUIRE(type_matches_tag<Select>(stmt.value));
        REQUIRE(get<Select>(stmt.value).where.cap == 1);
        REQUIRE(get<Select>(stmt.value).distinct == true);
    }

    SECTION("insert RDF triples") {
        InsertData ins{};
        ins.graph = "http://example.org/graph";
        push_back(ins.triples, TriplePattern{
            .subject   = Term{.kind = TermKind::iri, .value = AutoString8("http://example.org/s")},
            .predicate = Term{.kind = TermKind::iri, .value = AutoString8("http://example.org/p")},
            .object    = Term{.kind = TermKind::literal, .value = AutoString8("value"), .datatype = AutoString8("http://www.w3.org/2001/XMLSchema#string")},
        });

        Statement stmt{};
        stmt.value = move(ins);
        REQUIRE(type_matches_tag<InsertData>(stmt.value));
        REQUIRE(get<InsertData>(stmt.value).triples.length == 1);
    }
}
