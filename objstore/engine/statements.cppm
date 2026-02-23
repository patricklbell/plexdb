export module objstore.engine.statements;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import plexdb.tagged_union;

import objstore.engine.dtype;

using namespace plexdb;

export namespace objstore {
    constexpr U64 MAX_KEYSPACE_OPTIONS = 32;
    constexpr U64 MAX_COLUMN_NAMES = 64;
    constexpr U64 MAX_WHERE_RELATIONS = 32;
    constexpr U64 MAX_ASSIGNMENTS = 64;
    constexpr U64 MAX_BATCH_STATEMENTS = 128;

    // ========================================================================
    // common
    // ========================================================================
    struct TableRef {
        String8 keyspace_name;
        String8 table_name;
    };

    enum class ComparisonOp : U8 {
        eq,
        ne,
        lt,
        le,
        gt,
        ge,
        in_,
        contains,
        contains_key
    };

    struct WhereRelation {
        String8 column_name;
        ComparisonOp op;
        dtype::WriteValue value;
    };

    struct Assignment {
        String8 column_name;
        dtype::WriteValue value;
    };

    // ========================================================================
    // OPTION
    // ========================================================================
    struct KeyspaceOption {
        String8 key;
        TaggedUnion<AutoString8, S64> value;
    };

    // ========================================================================
    // USE
    // ========================================================================
    struct UseKeyspace {
        String8 keyspace_name;
    };

    // ========================================================================
    // CREATE KEYSPACE
    // ========================================================================
    struct CreateKeyspace {
        String8 keyspace_name;
        bool if_not_exists = false;
        CappedArray<KeyspaceOption, MAX_KEYSPACE_OPTIONS> options;
    };

    // ========================================================================
    // ALTER KEYSPACE
    // ========================================================================
    struct AlterKeyspace {
        String8 keyspace_name;
        bool if_exists = false;
        CappedArray<KeyspaceOption, MAX_KEYSPACE_OPTIONS> options;
    };

    // ========================================================================
    // DROP KEYSPACE
    // ========================================================================
    struct DropKeyspace {
        String8 keyspace_name;
        bool if_exists = false;
    };

    // ========================================================================
    // CREATE TABLE
    // ========================================================================
    struct CreateColumn {
        String8 name;
        DType dtype;
        bool is_primary_key = false;
    };

    struct CreateTable {
        String8 keyspace_name;
        String8 table_name;
        bool if_not_exists = false;
        DynamicArray<CreateColumn> columns;
    };

    // ========================================================================
    // ALTER TABLE
    // ========================================================================
    enum class AlterTableOp : U8 {
        add_column,
        drop_column,
        rename_column
    };

    struct AlterColumn {
        String8 name;
        DType dtype;  // only used for add_column
    };

    struct RenameColumn {
        String8 old_name;
        String8 new_name;
    };

    struct AlterTable {
        String8 keyspace_name;
        String8 table_name;
        bool if_exists = false;
        AlterTableOp op = AlterTableOp::add_column;
        // @note only one of these is active based on op
        // @todo tagged union
        DynamicArray<AlterColumn> columns;  // for add/drop
        DynamicArray<RenameColumn> renames; // for rename
    };

    // ========================================================================
    // DROP TABLE
    // ========================================================================
    struct DropTable {
        String8 keyspace_name;
        String8 table_name;
        bool if_exists = false;
    };

    // ========================================================================
    // TRUNCATE
    // ========================================================================
    struct TruncateTable {
        String8 keyspace_name;
        String8 table_name;
    };

    // ========================================================================
    // INSERT INTO
    // ========================================================================
    struct InsertInto {
        String8 keyspace_name;
        String8 table_name;
        bool if_not_exists = false;
        DynamicArray<String8> column_names;  // optional, if specified
        DynamicArray<dtype::WriteValue> values;
        S64 timestamp = -1;  // USING TIMESTAMP, -1 if not specified
        S64 ttl = -1;        // USING TTL, -1 if not specified
    };

    // ========================================================================
    // UPDATE
    // ========================================================================
    struct Update {
        String8 keyspace_name;
        String8 table_name;
        CappedArray<Assignment, MAX_ASSIGNMENTS> assignments;
        CappedArray<WhereRelation, MAX_WHERE_RELATIONS> where;
        bool if_exists = false;
        S64 timestamp = -1;  // USING TIMESTAMP, -1 if not specified
        S64 ttl = -1;        // USING TTL, -1 if not specified
    };

    // ========================================================================
    // DELETE
    // ========================================================================
    struct Delete {
        String8 keyspace_name;
        String8 table_name;
        CappedArray<String8, MAX_COLUMN_NAMES> column_names;  // columns to delete, empty = whole row
        CappedArray<WhereRelation, MAX_WHERE_RELATIONS> where;
        bool if_exists = false;
        S64 timestamp = -1;  // USING TIMESTAMP, -1 if not specified
    };

    // ========================================================================
    // SELECT FROM
    // ========================================================================
    struct SelectFrom {
        String8 keyspace_name;
        String8 table_name;
        CappedArray<String8, MAX_COLUMN_NAMES> column_names;  // empty = *
        CappedArray<WhereRelation, MAX_WHERE_RELATIONS> where;
        S64 limit = -1;  // -1 if not specified
        // @todo ORDER BY, GROUP BY, ALLOW FILTERING
    };

    // ========================================================================
    // BATCH
    // ========================================================================
    enum class BatchType : U8 {
        logged,
        unlogged,
        counter
    };

    struct BatchStatement {
        TaggedUnion<InsertInto, Update, Delete> value;
    };

    struct Batch {
        BatchType type = BatchType::logged;
        S64 timestamp = -1;  // USING TIMESTAMP, -1 if not specified
        DynamicArray<BatchStatement> statements;
    };

    // ========================================================================
    // statement union
    // ========================================================================
    struct Statement {
        TaggedUnion<
            UseKeyspace,
            CreateKeyspace,
            AlterKeyspace,
            DropKeyspace,
            CreateTable,
            AlterTable,
            DropTable,
            TruncateTable,
            InsertInto,
            Update,
            Delete,
            SelectFrom,
            Batch
        > value;
    };
}
