export module objstore.engine.it;

import plexdb.base;
import plexdb.pager;
import plexdb.btree;
import plexdb.blob;

import objstore.engine.io;
import objstore.engine.schema;

using namespace plexdb;

export namespace objstore {
    struct ColumnIterator {
    public:
        ColumnIterator();
        ColumnIterator(Pager* pager, const schema::Table* table, U64 page_idx);

        ColumnValue operator*();

        ColumnIterator& operator++();
        bool operator==(const ColumnIterator& other) const {
            return (table == nullptr && other.table == nullptr) ||
                   (table == other.table && current_column_idx == other.current_column_idx);
        }
        bool operator!=(const ColumnIterator& other) const { return !(*this == other); }

    private:
        const schema::Table* table = nullptr;
        blob::BlobDynamicPaged row_blob;
        U64 row_column_count = 0;
        U64 current_column_idx = 0;
        U64 current_byte_offset = 0;
        U64 current_mask = 0;
    };

    struct ColumnRange {
        ColumnIterator start;
        ColumnIterator stop;

        ColumnIterator& begin() { return start; }
        ColumnIterator& end() { return stop; }
    };

    struct RowIterator {
        Pager* pager = nullptr;
        schema::Table* table = nullptr;
        btree::Iterator<btree::BTreePaged, U64> pk_hash_it;

        ColumnRange operator*();

        RowIterator& operator++();

        bool operator==(const RowIterator& other) const {
            assert_true(table == other.table || table == nullptr || other.table == nullptr, "invalid iterator comparison");
            return pk_hash_it == other.pk_hash_it;
        }
        bool operator!=(const RowIterator& other) const { return !(*this == other); }
    };

    RowIterator create_table_begin_it(Pager* pager, schema::Table* table);
    RowIterator create_table_end_it(Pager* pager, schema::Table* table);
    RowIterator create_table_eq_it(Pager* pager, schema::Table* table, U64 pk_hash);
    RowIterator create_table_lt_it(Pager* pager, schema::Table* table, U64 pk_hash);
    RowIterator create_table_le_it(Pager* pager, schema::Table* table, U64 pk_hash);
    RowIterator create_table_gt_it(Pager* pager, schema::Table* table, U64 pk_hash);
    RowIterator create_table_ge_it(Pager* pager, schema::Table* table, U64 pk_hash);

    struct RowRange {
        RowIterator start;
        RowIterator stop;

        RowIterator& begin() { return start; }
        RowIterator& end() { return stop; }
    };
}
