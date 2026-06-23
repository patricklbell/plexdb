export module cql.engine.it;

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
import plexdb.pager;
import plexdb.btree;
import plexdb.blob;

import cql.engine.column_value;
import cql.engine.io;
import cql.engine.planner;
import cql.engine.schema;
import cql.engine.statements;

using namespace plexdb;

export namespace cql {
    // ColumnIterator streams column values one at a time from a BlobCursor.
    // Mask words are loaded upfront (tiny). deref() and advance() each open
    // and commit their own short transaction; skip of fixed-width columns is
    // pure arithmetic with no transaction.
    struct ColumnIterator {
    public:
        ColumnIterator() = default;

        coroutine::Task<ColumnValue> deref();
        coroutine::Task<void>        advance();

        bool operator==(const ColumnIterator& other) const {
            return (table == nullptr && other.table == nullptr) || (table == other.table && current_column_idx == other.current_column_idx);
        }
        bool operator!=(const ColumnIterator& other) const {
            return !(*this == other);
        }

        friend coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx);
        friend coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx, TArrayView<const U8, U16> pk_bytes);
        friend coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx, TArrayView<const U8, U16> pk_bytes, TArrayView<const U8, U16> ck_bytes);

        // @note row-blob metadata loaded by load(); inspect after load to enforce TTL expiry.
        io::RowMetadata metadata{};

    private:
        const schema::Table* table = nullptr;

        blob::BlobCursor row_cursor;
        blob::BlobCursor static_cursor; // valid() == false when no static page

        U64 row_column_count   = 0;
        U64 current_column_idx = 0;

        // All mask words loaded upfront in load(); safe to hold across transactions.
        DynamicArray<U64> masks;
        DynamicArray<U64> static_masks;

        // @note PK/CK values are not stored in the row blob; the with-keys load() overloads
        // decode them from the encoded key bytes and deref() returns them by key_position.
        DynamicArray<ColumnValue> injected_pk_values;
        DynamicArray<ColumnValue> injected_ck_values;

        // Set by deref() when it reads a non-null value, cleared by advance().
        // advance() skips bytes only when this is false (caller skipped deref).
        bool current_value_consumed = false;

        bool current_is_null() const {
            U64 word = current_column_idx / io::MASK_BIT_COUNT;
            U64 bit  = current_column_idx % io::MASK_BIT_COUNT;
            if (static_cast<bool>(static_cursor) && table->cols[current_column_idx].is_static) {
                return word >= static_masks.length || !(static_masks[word] & (1_u64 << bit));
            }
            return word >= masks.length || !(masks[word] & (1_u64 << bit));
        }

        bool current_is_static() const {
            return static_cast<bool>(static_cursor) && table->cols[current_column_idx].is_static;
        }
    };

    coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx = 0);
    coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx, TArrayView<const U8, U16> pk_bytes);
    coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx, TArrayView<const U8, U16> pk_bytes, TArrayView<const U8, U16> ck_bytes);

    struct ColumnRange {
        ColumnIterator start;
        ColumnIterator stop;

        ColumnIterator& begin() {
            return start;
        }
        ColumnIterator& end() {
            return stop;
        }
    };

    using PartitionBTree  = schema::PartitionBTree;
    using ClusteringBTree = schema::ClusteringBTree;

    struct RowIterator {
        Pager*         pager = nullptr;
        schema::Table* table = nullptr;

        btree::Iterator<PartitionBTree, schema::PartitionEntry> partition_it;

        ClusteringBTree                       clustering_btree;
        btree::Iterator<ClusteringBTree, U64> clustering_it;
        btree::Iterator<ClusteringBTree, U64> clustering_end_it;

        bool static_only_row = false; // @note true when partition has static data but no clustering rows

        // CK iteration bounds; applied per-partition in setup_clustering_for_partition.
        planner::KeyBounds ck;

        // @note when reverse_clustering is true, clustering_end_it is rend
        // (leaf=nullptr) and the lower bound is checked on each advance.
        bool reverse_clustering = false;

        RowIterator() = default;

        RowIterator(const RowIterator&)            = delete;
        RowIterator& operator=(const RowIterator&) = delete;

        RowIterator(RowIterator&& other) noexcept
            : pager(other.pager)
            , table(other.table)
            , partition_it(move(other.partition_it))
            , clustering_btree(other.clustering_btree)
            , clustering_it(move(other.clustering_it))
            , clustering_end_it(move(other.clustering_end_it))
            , static_only_row(other.static_only_row)
            , ck(move(other.ck))
            , reverse_clustering(other.reverse_clustering) {
            fix_clustering_btree_ptr(other);
        }

        RowIterator& operator=(RowIterator&& other) noexcept {
            if (this != &other) {
                pager              = other.pager;
                table              = other.table;
                partition_it       = move(other.partition_it);
                clustering_btree   = other.clustering_btree;
                clustering_it      = move(other.clustering_it);
                clustering_end_it  = move(other.clustering_end_it);
                static_only_row    = other.static_only_row;
                ck                 = move(other.ck);
                reverse_clustering = other.reverse_clustering;
                fix_clustering_btree_ptr(other);
            }
            return *this;
        }

        coroutine::Task<ColumnRange> deref();
        coroutine::Task<void>        advance(const RowIterator& stop);
        coroutine::Task<void>        advance_partition();

        bool operator==(const RowIterator& other) const {
            assert_true(table == other.table || table == nullptr || other.table == nullptr, "invalid iterator comparison");
            return partition_it == other.partition_it;
        }
        bool operator!=(const RowIterator& other) const {
            return !(*this == other);
        }

    private:
        void fix_clustering_btree_ptr(const RowIterator& src) {
            if (clustering_it.btree == &src.clustering_btree) {
                clustering_it.btree = &clustering_btree;
            }
            if (clustering_end_it.btree == &src.clustering_btree) {
                clustering_end_it.btree = &clustering_btree;
            }
        }
    };

    // @note must be called after assigning it.ck_* bounds and before iteration begins
    coroutine::Task<void> apply_ck_bounds_on_clustering(RowIterator& it);

    // @note skips leading partitions whose CK range contains no rows and no static data
    coroutine::Task<void> advance_past_empty_ck_partitions(RowIterator& it, const RowIterator& stop);

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
