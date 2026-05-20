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

    using PartitionBTree = schema::PartitionBTree;

    struct RowIterator {
        Pager* pager = nullptr;
        schema::Table* table = nullptr;

        // Partition-level iterator; value = row_page (no clustering) or clustering_btree_page (clustering)
        btree::Iterator<PartitionBTree, U64> partition_it;

        // Clustering-level (only when table->clustering_key_col_indices.length > 0).
        // clustering_btree is stored by value; clustering_it.btree points at &clustering_btree.
        // Custom copy/move maintain that invariant after object moves.
        PartitionBTree clustering_btree;
        btree::Iterator<PartitionBTree, U64> clustering_it;
        btree::Iterator<PartitionBTree, U64> clustering_end_it;

        RowIterator() = default;

        RowIterator(const RowIterator&) = delete;
        RowIterator& operator=(const RowIterator&) = delete;

        RowIterator(RowIterator&& other) noexcept
            : pager(other.pager)
            , table(other.table)
            , partition_it(move(other.partition_it))
            , clustering_btree(other.clustering_btree)
            , clustering_it(move(other.clustering_it))
            , clustering_end_it(move(other.clustering_end_it))
        {
            fix_clustering_btree_ptr(other);
        }

        RowIterator& operator=(RowIterator&& other) noexcept {
            if (this != &other) {
                pager = other.pager;
                table = other.table;
                partition_it = move(other.partition_it);
                clustering_btree = other.clustering_btree;
                clustering_it = move(other.clustering_it);
                clustering_end_it = move(other.clustering_end_it);
                fix_clustering_btree_ptr(other);
            }
            return *this;
        }

        coroutine::Task<ColumnRange> deref();
        coroutine::Task<void>        advance();

        bool operator==(const RowIterator& other) const {
            assert_true(table == other.table || table == nullptr || other.table == nullptr, "invalid iterator comparison");
            return partition_it == other.partition_it;
        }
        bool operator!=(const RowIterator& other) const { return !(*this == other); }

    private:
        // After copying/moving iterator fields, fix up clustering_it.btree so it points
        // to this->clustering_btree rather than src.clustering_btree.
        void fix_clustering_btree_ptr(const RowIterator& src) {
            if (clustering_it.btree == &src.clustering_btree)
                clustering_it.btree = &clustering_btree;
            if (clustering_end_it.btree == &src.clustering_btree)
                clustering_end_it.btree = &clustering_btree;
        }
    };

    RowIterator create_table_end_it(Pager* pager, schema::Table* table);

    coroutine::Task<RowIterator> create_table_begin_it(Pager* pager, schema::Table* table);
    coroutine::Task<RowIterator> create_table_eq_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key);
    coroutine::Task<RowIterator> create_table_lt_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key);
    coroutine::Task<RowIterator> create_table_le_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key);
    coroutine::Task<RowIterator> create_table_gt_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key);
    coroutine::Task<RowIterator> create_table_ge_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key);

    struct RowRange {
        RowIterator start;
        RowIterator stop;
    };
}
