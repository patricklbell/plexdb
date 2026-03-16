export module sqlstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.tagged_union;

using namespace plexdb;

export namespace sqlstore {
    constexpr U64 MAX_COLUMNS = 64;
    constexpr U64 MAX_WHERE_EXPRS = 32;
    constexpr U64 MAX_ORDER_BY = 8;
    constexpr U64 MAX_GROUP_BY = 8;
    constexpr U64 MAX_JOIN_EXPRS = 8;

    // ========================================================================
    // common
    // ========================================================================
    enum class DataType : U8 {
        integer,
        bigint,
        real_,
        double_,
        text,
        boolean,
        timestamp,
        // spatial types
        point,
        linestring,
        polygon,
        geometry,
    };

    enum class ComparisonOp : U8 { eq, ne, lt, le, gt, ge, like, in_, is_null, is_not_null };
    enum class SortOrder : U8 { asc, desc };
    enum class JoinType : U8 { inner, left_, right_, full_, cross_ };

    struct TableRef {
        String8 schema_name;
        String8 table_name;
    };

    struct ColumnDef {
        String8 name;
        DataType type;
        bool not_null = false;
        bool primary_key = false;
        bool spatial_index = false;
    };

    struct WhereExpr {
        String8 column_name;
        ComparisonOp op;
        AutoString8 value;
    };

    struct OrderByClause {
        String8 column_name;
        SortOrder order = SortOrder::asc;
    };

    struct JoinClause {
        JoinType type;
        TableRef table;
        String8 left_column;
        String8 right_column;
    };

    struct Assignment {
        String8 column_name;
        AutoString8 value;
    };

    // ========================================================================
    // CREATE SCHEMA / DROP SCHEMA
    // ========================================================================
    struct CreateSchema { String8 name; bool if_not_exists = false; };
    struct DropSchema { String8 name; bool if_exists = false; };

    // ========================================================================
    // CREATE TABLE
    // ========================================================================
    struct CreateTable {
        String8 schema_name;
        String8 table_name;
        bool if_not_exists = false;
        DynamicArray<ColumnDef> columns;
    };

    // ========================================================================
    // DROP TABLE
    // ========================================================================
    struct DropTable {
        String8 schema_name;
        String8 table_name;
        bool if_exists = false;
    };

    // ========================================================================
    // ALTER TABLE
    // ========================================================================
    struct AlterTableAddColumn {
        String8 schema_name;
        String8 table_name;
        ColumnDef column;
    };

    struct AlterTableDropColumn {
        String8 schema_name;
        String8 table_name;
        String8 column_name;
    };

    // ========================================================================
    // CREATE INDEX
    // ========================================================================
    struct CreateIndex {
        String8 index_name;
        String8 schema_name;
        String8 table_name;
        CappedArray<String8, MAX_COLUMNS> columns;
        bool spatial = false;
    };

    // ========================================================================
    // SELECT
    // ========================================================================
    struct Select {
        String8 schema_name;
        String8 table_name;
        CappedArray<String8, MAX_COLUMNS> columns;  // empty = *
        CappedArray<WhereExpr, MAX_WHERE_EXPRS> where;
        CappedArray<JoinClause, MAX_JOIN_EXPRS> joins;
        CappedArray<OrderByClause, MAX_ORDER_BY> order_by;
        CappedArray<String8, MAX_GROUP_BY> group_by;
        S64 limit = -1;
        S64 offset = -1;
    };

    // ========================================================================
    // INSERT
    // ========================================================================
    struct InsertInto {
        String8 schema_name;
        String8 table_name;
        DynamicArray<String8> column_names;
        DynamicArray<AutoString8> values;
    };

    // ========================================================================
    // UPDATE
    // ========================================================================
    struct Update {
        String8 schema_name;
        String8 table_name;
        DynamicArray<Assignment> assignments;
        CappedArray<WhereExpr, MAX_WHERE_EXPRS> where;
    };

    // ========================================================================
    // DELETE
    // ========================================================================
    struct Delete {
        String8 schema_name;
        String8 table_name;
        CappedArray<WhereExpr, MAX_WHERE_EXPRS> where;
    };

    // ========================================================================
    // statement union
    // ========================================================================
    struct Statement {
        TaggedUnion<
            CreateSchema, DropSchema,
            CreateTable, DropTable,
            AlterTableAddColumn, AlterTableDropColumn,
            CreateIndex,
            Select, InsertInto, Update, Delete
        > value;
    };
}
