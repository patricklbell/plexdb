module;
#include <coroutine>
#include <plexdb/support/tracy/tracy.hpp>

module cql.engine.it;

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
import plexdb.pager;
import plexdb.btree;
import plexdb.btree.types;
import plexdb.blob;

import cql.engine.io;
import cql.engine.schema;
import cql.engine.types;
import cql.engine.statements;

namespace cql {
    // ========================================================================
    // column iterator
    // ========================================================================


    static coroutine::Task<U64> read_blob_header(
        blob::BlobDynamicPaged& b, U64& out_col_count, DynamicArray<U64>& out_masks
    ) {
        co_await blob::get(b, reinterpret_cast<U8*>(&out_col_count),
                           io::COLUMN_COUNT_BYTE_COUNT, 0);
        U64 mask_words = ceil_div(out_col_count, io::MASK_BIT_COUNT);
        resize(out_masks, mask_words);
        if (mask_words > 0) {
            co_await blob::get(b, reinterpret_cast<U8*>(out_masks.ptr),
                               mask_words * io::MASK_BYTE_COUNT, io::COLUMN_COUNT_BYTE_COUNT);
        }
        co_return io::COLUMN_COUNT_BYTE_COUNT + mask_words * io::MASK_BYTE_COUNT;
    }

    coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table,
                           U64 page_idx, U64 static_page_idx) {
        it.table = table;
        assert_true(it.table->cols.length != 0,
                    "column cannot be empty, it must at least have a PK");

        // Open cursors and read all mask words. Opens its own short transaction
        // if none is currently active; otherwise uses the caller's transaction.
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();

            it.row_cursor = co_await blob::create_cursor(pager, page_idx);
            U64 data_offset = co_await read_blob_header(
                it.row_cursor.blob, it.row_column_count, it.masks);
            it.row_cursor.offset = data_offset;

            if (static_page_idx != 0 && table->static_col_indices.length > 0) {
                it.static_cursor = co_await blob::create_cursor(pager, static_page_idx);
                U64 static_col_count = 0;
                U64 static_data_offset = co_await read_blob_header(
                    it.static_cursor.blob, static_col_count, it.static_masks);
                it.static_cursor.offset = static_data_offset;
            }

            if (own_tx) co_await tx.commit();
        }

        it.current_column_idx    = 0;
        it.current_value_consumed = false;
    }

    coroutine::Task<ColumnValue> ColumnIterator::deref() { ZoneScopedN("it::column_read");
        assert_true(this->table != nullptr,
                    "cannot dereference an end iterator, this should never happen!");

        if (this->current_column_idx >= this->row_column_count)
            co_return ColumnValue{Null{}};

        if (this->current_is_null())
            co_return ColumnValue{Null{}};

        blob::BlobCursor& cursor = this->current_is_static()
            ? this->static_cursor : this->row_cursor;

        auto r_fn = [&cursor](U8* dst, U64 size) -> coroutine::Task<void> {
            co_await blob::read(cursor, dst, size);
        };
        ColumnValue result = co_await io::read_column_value(io::to_reader(r_fn), this->table->cols[this->current_column_idx].type);
        this->current_value_consumed = true;
        co_return result;
    }

    coroutine::Task<void> ColumnIterator::advance() {
        assert_true(this->table != nullptr, "cannot advance end iterator");

        if (this->current_column_idx >= this->row_column_count) {
            this->table = nullptr;
            co_return;
        }

        if (!this->current_is_null() && !this->current_value_consumed) {
            blob::BlobCursor& cursor = this->current_is_static()
                ? this->static_cursor : this->row_cursor;
            auto r_fn = [&cursor](U8* dst, U64 size) -> coroutine::Task<void> {
                if (dst != nullptr) co_await blob::read(cursor, dst, size);
                else cursor.skip(size);
            };
            co_await io::skip_column_value(io::to_reader(r_fn), this->table->cols[this->current_column_idx].type);
        }
        this->current_value_consumed = false;
        this->current_column_idx++;

        if (this->current_column_idx >= this->row_column_count)
            this->table = nullptr;
    }

    // ========================================================================
    // row iterator
    // ========================================================================

    coroutine::Task<ColumnRange> RowIterator::deref() {
        auto partition_entry = *partition_it;
        U64 row_page = schema::has_clustering_keys(*table)
            ? *clustering_it
            : partition_entry.data_page;
        ColumnIterator col_it;
        co_await load(col_it, this->pager, this->table, row_page, partition_entry.static_page);
        co_return ColumnRange{
            .start = move(col_it),
            .stop  = ColumnIterator{},
        };
    }

    coroutine::Task<void> RowIterator::advance() {
        bool own_tx = !this->pager->transaction_active;
        pager::Transaction tx{this->pager};
        if (own_tx) co_await tx.begin();
        if (!schema::has_clustering_keys(*table)) {
            co_await this->partition_it.advance();
        } else {
            co_await this->clustering_it.advance();
            if (clustering_it == clustering_end_it) {
                co_await this->partition_it.advance();
                auto end_it = btree::end<schema::PartitionEntry>(table->btree);
                if (partition_it != end_it) {
                    auto entry = *partition_it;
                    clustering_btree = ClusteringBTree{
                        pager, entry.data_page,
                        btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}
                    };
                    clustering_it     = co_await btree::begin<U64>(clustering_btree);
                    clustering_end_it = btree::end<U64>(clustering_btree);
                }
            }
        }
        if (own_tx) co_await tx.commit();
    }

    static coroutine::Task<void> create_clustering(RowIterator& it, Pager* pager, schema::Table* table) {
        if (schema::has_clustering_keys(*table) &&
            it.partition_it != btree::end<schema::PartitionEntry>(table->btree)) {
            auto entry = *it.partition_it;
            it.clustering_btree = ClusteringBTree{
                pager, entry.data_page,
                btree::VarlenKeyPolicy<>{}, btree::FixedValuePolicy<sizeof(U64)>{}
            };
            it.clustering_it     = co_await btree::begin<U64>(it.clustering_btree);
            it.clustering_end_it = btree::end<U64>(it.clustering_btree);
        }
    }

    RowIterator create_table_end_it(Pager* pager, schema::Table* table) {
        RowIterator it;
        it.pager        = pager;
        it.table        = table;
        it.partition_it = btree::end<schema::PartitionEntry>(table->btree);
        return it;
    }

    coroutine::Task<RowIterator> create_table_begin_it(Pager* pager, schema::Table* table) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();
            it.partition_it = co_await btree::begin<schema::PartitionEntry>(table->btree);
            co_await create_clustering(it, pager, table);
            if (own_tx) co_await tx.commit();
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_eq_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::RequireEquality>(table->btree, pk_key);
            co_await create_clustering(it, pager, table);
            if (own_tx) co_await tx.commit();
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_lt_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_key);
            if (own_tx) co_await tx.commit();
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_le_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreater>(table->btree, pk_key);
            if (own_tx) co_await tx.commit();
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_gt_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreater>(table->btree, pk_key);
            co_await create_clustering(it, pager, table);
            if (own_tx) co_await tx.commit();
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_ge_it(Pager* pager, schema::Table* table, TArrayView<const U8, U16> pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) co_await tx.begin();
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_key);
            co_await create_clustering(it, pager, table);
            if (own_tx) co_await tx.commit();
        }
        co_return it;
    }
}
