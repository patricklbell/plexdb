export module cql.engine.it;

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
import plexdb.pager;
import plexdb.btree;
import plexdb.blob;

import cql.engine.io;
import cql.engine.schema;

using namespace plexdb;

export namespace cql {
    // ColumnIterator pre-fetches all row bytes on construction so operator*/++ are sync.
    struct ColumnIterator {
    public:
        ColumnIterator() = default;

        ColumnValue operator*();
        ColumnIterator& operator++();

        bool operator==(const ColumnIterator& other) const {
            return (table == nullptr && other.table == nullptr) ||
                   (table == other.table && current_column_idx == other.current_column_idx);
        }
        bool operator!=(const ColumnIterator& other) const { return !(*this == other); }

        friend coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx);

    private:
        const schema::Table* table = nullptr;
        DynamicArray<U8> row_data;
        U64 row_column_count = 0;
        U64 current_column_idx = 0;
        U64 current_byte_offset = 0;  // absolute byte position into row_data for current column data
        U64 current_mask = 0;
    };

    coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx);

    struct ColumnRange {
        ColumnIterator start;
        ColumnIterator stop;

        ColumnIterator& begin() { return start; }
        ColumnIterator& end() { return stop; }
    };

    struct RowIterator {
        Pager* pager = nullptr;
        schema::Table* table = nullptr;
        btree::Iterator<btree::BTreePaged<btree::FixedKeyPolicy<U64>, btree::FixedValuePolicy<sizeof(U64)>>, U64> pk_hash_it;

        coroutine::Task<ColumnRange> deref();
        coroutine::Task<void>        advance();

        bool operator==(const RowIterator& other) const {
            assert_true(table == other.table || table == nullptr || other.table == nullptr, "invalid iterator comparison");
            return pk_hash_it == other.pk_hash_it;
        }
        bool operator!=(const RowIterator& other) const { return !(*this == other); }
    };

    // end iterator is sync (btree::tend is sync)
    RowIterator create_table_end_it(Pager* pager, schema::Table* table);

    coroutine::Task<RowIterator> create_table_begin_it(Pager* pager, schema::Table* table);
    coroutine::Task<RowIterator> create_table_eq_it(Pager* pager, schema::Table* table, U64 pk_hash);
    coroutine::Task<RowIterator> create_table_lt_it(Pager* pager, schema::Table* table, U64 pk_hash);
    coroutine::Task<RowIterator> create_table_le_it(Pager* pager, schema::Table* table, U64 pk_hash);
    coroutine::Task<RowIterator> create_table_gt_it(Pager* pager, schema::Table* table, U64 pk_hash);
    coroutine::Task<RowIterator> create_table_ge_it(Pager* pager, schema::Table* table, U64 pk_hash);

    struct RowRange {
        RowIterator start;
        RowIterator stop;
    };
}
