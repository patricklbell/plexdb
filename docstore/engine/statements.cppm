export module docstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.tagged_union;

using namespace plexdb;

export namespace docstore {
    constexpr U64 MAX_FILTERS = 32;
    constexpr U64 MAX_FIELDS = 64;
    constexpr U64 MAX_DOCUMENTS = 128;

    // ========================================================================
    // common
    // ========================================================================
    struct CollectionRef {
        String8 database_name;
        String8 collection_name;
    };

    enum class FilterOp : U8 {
        eq,
        ne,
        lt,
        le,
        gt,
        ge,
        exists,
        in_,
        regex
    };

    struct FilterExpr {
        String8 field;
        FilterOp op;
        AutoString8 value;
    };

    // ========================================================================
    // CREATE DATABASE
    // ========================================================================
    struct CreateDatabase {
        String8 name;
    };

    // ========================================================================
    // DROP DATABASE
    // ========================================================================
    struct DropDatabase {
        String8 name;
        bool if_exists = false;
    };

    // ========================================================================
    // CREATE COLLECTION
    // ========================================================================
    struct CreateCollection {
        String8 database_name;
        String8 collection_name;
        bool if_not_exists = false;
    };

    // ========================================================================
    // DROP COLLECTION
    // ========================================================================
    struct DropCollection {
        String8 database_name;
        String8 collection_name;
        bool if_exists = false;
    };

    // ========================================================================
    // INSERT
    // ========================================================================
    struct InsertDocument {
        String8 database_name;
        String8 collection_name;
        DynamicArray<AutoString8> documents;
    };

    // ========================================================================
    // FIND
    // ========================================================================
    struct FindDocuments {
        String8 database_name;
        String8 collection_name;
        CappedArray<FilterExpr, MAX_FILTERS> filter;
        CappedArray<String8, MAX_FIELDS> projection;
        S64 limit = -1;
        S64 skip = 0;
    };

    // ========================================================================
    // UPDATE
    // ========================================================================
    struct UpdateDocuments {
        String8 database_name;
        String8 collection_name;
        CappedArray<FilterExpr, MAX_FILTERS> filter;
        AutoString8 update;
        bool multi = false;
    };

    // ========================================================================
    // DELETE
    // ========================================================================
    struct DeleteDocuments {
        String8 database_name;
        String8 collection_name;
        CappedArray<FilterExpr, MAX_FILTERS> filter;
        bool multi = false;
    };

    // ========================================================================
    // statement union
    // ========================================================================
    struct Statement {
        TaggedUnion<
            CreateDatabase,
            DropDatabase,
            CreateCollection,
            DropCollection,
            InsertDocument,
            FindDocuments,
            UpdateDocuments,
            DeleteDocuments
        > value;
    };
}
