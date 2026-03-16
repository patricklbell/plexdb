export module graphstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.tagged_union;

using namespace plexdb;

export namespace graphstore {
    constexpr U64 MAX_VARIABLES = 32;
    constexpr U64 MAX_TRIPLE_PATTERNS = 32;
    constexpr U64 MAX_PREFIXES = 16;
    constexpr U64 MAX_ORDER_BY = 8;

    enum class TermKind : U8 { iri, literal, variable, blank_node };

    struct Term {
        TermKind kind;
        AutoString8 value;
        AutoString8 datatype;
        AutoString8 language;
    };

    struct TriplePattern {
        Term subject;
        Term predicate;
        Term object;
    };

    struct Prefix {
        String8 label;
        AutoString8 iri;
    };

    enum class SortOrder : U8 { asc, desc };

    struct OrderByClause {
        String8 variable;
        SortOrder order = SortOrder::asc;
    };

    // SPARQL SELECT
    struct Select {
        CappedArray<Prefix, MAX_PREFIXES> prefixes;
        CappedArray<String8, MAX_VARIABLES> variables;
        CappedArray<TriplePattern, MAX_TRIPLE_PATTERNS> where;
        CappedArray<OrderByClause, MAX_ORDER_BY> order_by;
        S64 limit = -1;
        S64 offset = -1;
        bool distinct = false;
    };

    // SPARQL CONSTRUCT
    struct Construct {
        CappedArray<Prefix, MAX_PREFIXES> prefixes;
        CappedArray<TriplePattern, MAX_TRIPLE_PATTERNS> template_;
        CappedArray<TriplePattern, MAX_TRIPLE_PATTERNS> where;
    };

    // SPARQL ASK
    struct Ask {
        CappedArray<Prefix, MAX_PREFIXES> prefixes;
        CappedArray<TriplePattern, MAX_TRIPLE_PATTERNS> where;
    };

    // SPARQL DESCRIBE
    struct Describe {
        CappedArray<Prefix, MAX_PREFIXES> prefixes;
        CappedArray<Term, MAX_VARIABLES> resources;
    };

    // SPARQL INSERT DATA
    struct InsertData {
        CappedArray<Prefix, MAX_PREFIXES> prefixes;
        String8 graph;
        DynamicArray<TriplePattern> triples;
    };

    // SPARQL DELETE DATA
    struct DeleteData {
        CappedArray<Prefix, MAX_PREFIXES> prefixes;
        String8 graph;
        DynamicArray<TriplePattern> triples;
    };

    // SPARQL CREATE GRAPH
    struct CreateGraph {
        AutoString8 graph_iri;
        bool if_not_exists = false;
    };

    // SPARQL DROP GRAPH
    struct DropGraph {
        AutoString8 graph_iri;
        bool if_exists = false;
    };

    struct Statement {
        TaggedUnion<
            Select, Construct, Ask, Describe,
            InsertData, DeleteData,
            CreateGraph, DropGraph
        > value;
    };
}
