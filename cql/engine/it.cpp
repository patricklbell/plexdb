module;
#include <coroutine>
#include <cstring>
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
import cql.engine.key;
import cql.engine.schema;
import cql.engine.types;
import cql.engine.statements;

namespace cql {
    // ========================================================================
    // column iterator
    // ========================================================================

    static coroutine::Task<U64> read_blob_header(
        blob::BlobDynamicPaged& b,
        io::RowMetadata&        out_meta,
        U64& out_col_count, DynamicArray<U64>& out_masks, DynamicArray<U64>& out_cell_meta_mask,
        U64 base_offset = 0
    ) {
        co_await blob::get(b, reinterpret_cast<U8*>(&out_meta.flags), sizeof(out_meta.flags), base_offset);
        co_await blob::get(b, reinterpret_cast<U8*>(&out_meta.expiry_unix_ms), sizeof(out_meta.expiry_unix_ms), base_offset + sizeof(out_meta.flags));
        co_await blob::get(b, reinterpret_cast<U8*>(&out_col_count), io::COLUMN_COUNT_BYTE_COUNT, base_offset + io::ROW_METADATA_BYTES);
        U64 mask_words = ceil_div(out_col_count, io::MASK_BIT_COUNT);
        resize(out_masks, mask_words);
        resize(out_cell_meta_mask, mask_words);
        U64 off = base_offset + io::ROW_METADATA_BYTES + io::COLUMN_COUNT_BYTE_COUNT;
        if (mask_words > 0) {
            co_await blob::get(b, reinterpret_cast<U8*>(out_masks.ptr), mask_words * io::MASK_BYTE_COUNT, off);
            off += mask_words * io::MASK_BYTE_COUNT;
            co_await blob::get(b, reinterpret_cast<U8*>(out_cell_meta_mask.ptr), mask_words * io::MASK_BYTE_COUNT, off);
            off += mask_words * io::MASK_BYTE_COUNT;
        }
        co_return off;
    }

    // Reads the partition-key preamble shared by both pk-preamble locations (see
    // rewrite_static / write_row_blob in engine.cpp): one io::write_column_value cell per
    // partition-key column, in key_position order, no mask (pk values are never null).
    static coroutine::Task<void> read_pk_preamble(io::Reader r, const schema::Table* table, DynamicArray<ColumnValue>& out) {
        for (U64 pk_ci : table->partition_key_col_indices) {
            ColumnValue v = co_await io::read_column_value(r, table->cols[pk_ci].type);
            push_back(out, move(v));
        }
    }

    coroutine::Task<> load(ColumnIterator& it, Pager* pager, const schema::Table* table, U64 page_idx, U64 static_page_idx, TArrayView<const U8, U16> ck_bytes) {
        it.table = table;
        assert_true(it.table->cols.length != 0, "column cannot be empty, it must at least have a PK");

        // Open cursors and read all mask words. Opens its own short transaction
        // if none is currently active; otherwise uses the caller's transaction.
        bool               own_tx = !pager->transaction_active;
        pager::Transaction tx{pager};
        if (own_tx) {
            co_await tx.begin();
        }

        it.row_column_count   = table->cols.length;
        it.metadata           = {};
        it.static_metadata    = {};
        it.injected_pk_values = {};
        // Tables without a clustering key can never have static columns and have exactly one
        // row blob per partition, so their pk preamble is prepended to that row blob instead
        // of a separate static page (see engine.cpp's write_row_blob) — avoids doubling the
        // per-partition page count for such tables.
        bool ck_less = !schema::has_clustering_keys(*table);
        if (page_idx != 0) {
            it.row_cursor = co_await blob::create_cursor(pager, page_idx);
            if (ck_less) {
                auto pk_r_fn = [&it](U8* dst, U64 size) -> coroutine::Task<void> {
                    co_await blob::read(it.row_cursor, dst, size);
                };
                co_await read_pk_preamble(io::to_reader(pk_r_fn), table, it.injected_pk_values);
            }
            U64 data_offset = co_await read_blob_header(
                it.row_cursor.blob, it.metadata, it.row_column_count, it.masks, it.cell_meta_mask, it.row_cursor.offset
            );
            it.row_cursor.offset = data_offset;
        }

        if (static_page_idx != 0 && !ck_less) {
            it.static_cursor = co_await blob::create_cursor(pager, static_page_idx);
            // Every live partition's static page starts with a partition-key preamble (one
            // io::write_column_value cell per partition-key column, in key_position order, no
            // mask — pk values are never null). The static-row-header section (RowMetadata +
            // masks + static column cells) follows only when the table has static columns.
            auto pk_r_fn = [&it](U8* dst, U64 size) -> coroutine::Task<void> {
                co_await blob::read(it.static_cursor, dst, size);
            };
            co_await read_pk_preamble(io::to_reader(pk_r_fn), table, it.injected_pk_values);
            // The static-row-header section is only present when rewrite_static wrote one
            // (i.e. the partition has at least one present static value) — detect that from
            // the blob's actual size rather than the table's schema, since a partition with
            // no static values ever set has a static page holding only the pk preamble.
            if (it.static_cursor.offset < it.static_cursor.blob.size_bytes) {
                U64 static_col_count   = 0;
                U64 static_data_offset = co_await read_blob_header(
                    it.static_cursor.blob, it.static_metadata, static_col_count, it.static_masks, it.static_cell_meta_mask,
                    it.static_cursor.offset
                );
                it.static_cursor.offset = static_data_offset;
            }
        }

        if (own_tx) {
            co_await tx.commit();
        }

        // Empty for clustering-less tables (decode_clustering is then a no-op) or when the
        // caller only wants the static/pk-preamble portion, not a specific clustering row.
        it.injected_ck_values     = key::decode_clustering(*table, ck_bytes);
        it.current_column_idx     = 0;
        it.current_value_consumed = false;
    }

    static io::CellMetadata derive_cell_meta_from_row(const io::RowMetadata& rm) {
        io::CellMetadata cm{};
        if (io::row_has_ttl(rm)) {
            cm.flags |= io::CELL_FLAG_HAS_TTL;
            cm.expiry_unix_ms = rm.expiry_unix_ms;
        }
        return cm;
    }

    coroutine::Task<ColumnValue> ColumnIterator::deref() {
        ZoneScopedN("it::column_read");
        assert_true(this->table != nullptr, "cannot dereference an end iterator, this should never happen!");

        if (this->current_column_idx >= this->row_column_count) {
            this->current_cell_metadata = {};
            co_return ColumnValue{Null{}};
        }

        if (this->current_is_null()) {
            this->current_cell_metadata = {};
            const schema::Column& col   = this->table->cols[this->current_column_idx];
            if (col.key_kind == schema::KeyKind::PartitionKey && col.key_position < this->injected_pk_values.length) {
                this->current_value_consumed = true;
                co_return this->injected_pk_values[col.key_position];
            }
            if (col.key_kind == schema::KeyKind::ClusteringKey && col.key_position < this->injected_ck_values.length) {
                this->current_value_consumed = true;
                co_return this->injected_ck_values[col.key_position];
            }
            co_return ColumnValue{Null{}};
        }

        bool              is_static = this->current_is_static();
        blob::BlobCursor& cursor    = is_static ? this->static_cursor : this->row_cursor;

        auto r_fn = [&cursor](U8* dst, U64 size) -> coroutine::Task<void> {
            co_await blob::read(cursor, dst, size);
        };
        if (this->current_has_cell_meta()) {
            this->current_cell_metadata = co_await io::read_cell_metadata(io::to_reader(r_fn));
        } else {
            this->current_cell_metadata = derive_cell_meta_from_row(is_static ? this->static_metadata : this->metadata);
        }
        ColumnValue result           = co_await io::read_column_value(io::to_reader(r_fn), this->table->cols[this->current_column_idx].type);
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
                                         ? this->static_cursor
                                         : this->row_cursor;
            auto              r_fn   = [&cursor](U8* dst, U64 size) -> coroutine::Task<void> {
                if (dst != nullptr) {
                    co_await blob::read(cursor, dst, size);
                } else {
                    cursor.skip(size);
                }
            };
            if (this->current_has_cell_meta()) {
                U8 flags;
                co_await r_fn(&flags, sizeof(flags));
                co_await io::skip_cell_metadata(io::to_reader(r_fn), flags);
            }
            co_await io::skip_column_value(io::to_reader(r_fn), this->table->cols[this->current_column_idx].type);
        }
        this->current_value_consumed = false;
        this->current_column_idx++;

        if (this->current_column_idx >= this->row_column_count) {
            this->table = nullptr;
        }
    }

    // ========================================================================
    // row iterator
    // ========================================================================

    coroutine::Task<ColumnRange> RowIterator::deref() {
        auto           partition_entry = *partition_it;
        ColumnIterator col_it;
        if (schema::has_clustering_keys(*table)) {
            if (static_only_row) {
                co_await load(col_it, this->pager, this->table, 0, partition_entry.static_page);
            } else {
                co_await load(col_it, this->pager, this->table, *clustering_it, partition_entry.static_page, clustering_it.key());
            }
        } else {
            co_await load(col_it, this->pager, this->table, partition_entry.data_page, partition_entry.static_page);
        }
        co_return ColumnRange{
            .start = move(col_it),
            .stop  = ColumnIterator{},
        };
    }

    static bool ck_key_has_prefix(TArrayView<const U8, U16> key, const DynamicArray<U8>& bound) {
        if (key.length < static_cast<U16>(bound.length)) {
            return false;
        }
        return bound.length == 0 || memcmp(key.ptr, bound.ptr, bound.length) == 0;
    }

    static coroutine::Task<void> setup_clustering_for_partition(RowIterator& it, Pager* pager, const schema::PartitionEntry& entry) {
        it.clustering_btree = ClusteringBTree{
            pager, entry.data_page,
            schema::make_clustering_key_policy(*it.table), btree::FixedValuePolicy<sizeof(U64)>{}
        };

        if (it.reverse_clustering) {
            // Position the start at the highest key satisfying the upper bound. For
            // partial upper bounds we start at rbegin and skip backward while the key
            // is past the bound.
            // @profile O(rows past upper bound). Acceptable while there is no
            // first-greater-on-prefix BTree primitive.
            if (it.ck.has_end && !it.ck.end_is_partial) {
                auto ev = TArrayView<const U8, U16>(it.ck.end.ptr, static_cast<U16>(it.ck.end.length));
                if (it.ck.end_inclusive) {
                    it.clustering_it = co_await btree::find_it<U64, btree::SearchStrategy::LastLessEqual>(it.clustering_btree, ev);
                } else {
                    it.clustering_it = co_await btree::find_it<U64, btree::SearchStrategy::LastLess>(it.clustering_btree, ev);
                }
            } else {
                it.clustering_it = co_await btree::rbegin<U64>(it.clustering_btree);
            }
            it.clustering_it.reverse = true;
            it.clustering_end_it     = btree::rend<U64>(it.clustering_btree);

            if (it.ck.has_end && it.ck.end_is_partial) {
                auto end_view = TArrayView<const U8, U16>(it.ck.end.ptr, static_cast<U16>(it.ck.end.length));
                while (it.clustering_it != it.clustering_end_it) {
                    Ordering ord      = key::compare_clustering(*it.table, it.clustering_it.key(), end_view);
                    bool     past_end = it.ck.end_inclusive ? (ord == Ordering::Greater) : (ord != Ordering::Less);
                    if (!past_end) {
                        break;
                    }
                    co_await it.clustering_it.advance();
                }
            }

            // If the chosen start already violates the lower bound, mark exhausted.
            if (it.clustering_it != it.clustering_end_it && it.ck.has_begin) {
                auto     begin_view = TArrayView<const U8, U16>(it.ck.begin.ptr, static_cast<U16>(it.ck.begin.length));
                Ordering ord        = key::compare_clustering(*it.table, it.clustering_it.key(), begin_view);
                bool     past_begin = it.ck.begin_inclusive ? (ord == Ordering::Less) : (ord != Ordering::Greater);
                if (past_begin) {
                    it.clustering_it = btree::rend<U64>(it.clustering_btree);
                }
            }

            it.static_only_row = (it.clustering_it == it.clustering_end_it && entry.static_page != 0);
            co_return;
        }

        if (it.ck.has_begin) {
            auto bv = TArrayView<const U8, U16>(it.ck.begin.ptr, static_cast<U16>(it.ck.begin.length));
            if (it.ck.begin_inclusive) {
                it.clustering_it = co_await btree::find_it<U64, btree::SearchStrategy::FirstGreaterEqual>(it.clustering_btree, bv);
            } else {
                it.clustering_it = co_await btree::find_it<U64, btree::SearchStrategy::FirstGreater>(it.clustering_btree, bv);
            }
            // @note composite keys byte-extending the prefix compare greater; loop to skip them
            if (it.ck.begin_is_partial && !it.ck.begin_inclusive) {
                auto end_it = btree::end<U64>(it.clustering_btree);
                while (it.clustering_it != end_it && ck_key_has_prefix(it.clustering_it.key(), it.ck.begin)) {
                    co_await it.clustering_it.advance();
                }
            }
        } else {
            it.clustering_it = co_await btree::begin<U64>(it.clustering_btree);
        }

        if (it.ck.has_end && !it.ck.end_is_partial) {
            auto ev = TArrayView<const U8, U16>(it.ck.end.ptr, static_cast<U16>(it.ck.end.length));
            if (it.ck.end_inclusive) {
                it.clustering_end_it = co_await btree::find_it<U64, btree::SearchStrategy::FirstGreater>(it.clustering_btree, ev);
            } else {
                it.clustering_end_it = co_await btree::find_it<U64, btree::SearchStrategy::FirstGreaterEqual>(it.clustering_btree, ev);
            }
        } else {
            it.clustering_end_it = btree::end<U64>(it.clustering_btree);
        }

        it.static_only_row = (it.clustering_it == it.clustering_end_it && entry.static_page != 0);
    }

    coroutine::Task<void> RowIterator::advance(const RowIterator& stop) {
        bool               own_tx = !this->pager->transaction_active;
        pager::Transaction tx{this->pager};
        if (own_tx) {
            co_await tx.begin();
        }
        if (!schema::has_clustering_keys(*table)) {
            co_await this->partition_it.advance();
        } else if (this->static_only_row) {
            this->static_only_row = false;
            co_await this->partition_it.advance();
            auto end_it = btree::end<schema::PartitionEntry>(table->btree);
            if (partition_it != end_it) {
                auto entry = *partition_it;
                co_await setup_clustering_for_partition(*this, pager, entry);
            }
        } else {
            co_await this->clustering_it.advance();
            if (this->reverse_clustering) {
                // For reverse iteration, clustering_end_it is rend; we instead check the
                // lower bound on each step.
                if (this->ck.has_begin && clustering_it != clustering_end_it) {
                    auto     begin_view = TArrayView<const U8, U16>(this->ck.begin.ptr, static_cast<U16>(this->ck.begin.length));
                    Ordering ord        = key::compare_clustering(*table, clustering_it.key(), begin_view);
                    bool     past_begin = this->ck.begin_inclusive ? (ord == Ordering::Less) : (ord != Ordering::Greater);
                    if (past_begin) {
                        clustering_it = btree::rend<U64>(clustering_btree);
                    }
                }
            } else {
                // @note ck_end_is_partial: clustering_end_it is btree::end; check the bound
                // on each step since it can't be found by position alone.
                if (this->ck.has_end && this->ck.end_is_partial && clustering_it != clustering_end_it) {
                    auto     end_view = TArrayView<const U8, U16>(this->ck.end.ptr, static_cast<U16>(this->ck.end.length));
                    Ordering ord      = key::compare_clustering(*table, clustering_it.key(), end_view);
                    bool     past_end = this->ck.end_inclusive ? (ord == Ordering::Greater) : (ord != Ordering::Less);
                    if (past_end) {
                        clustering_it = btree::end<U64>(clustering_btree);
                    }
                }
            }
            if (clustering_it == clustering_end_it) {
                auto end_it = btree::end<schema::PartitionEntry>(table->btree);
                do {
                    co_await this->partition_it.advance();
                    if (partition_it != end_it) {
                        auto entry = *partition_it;
                        co_await setup_clustering_for_partition(*this, pager, entry);
                    }
                } while (partition_it != end_it && partition_it != stop.partition_it && !static_only_row && clustering_it == clustering_end_it);
            }
        }
        if (own_tx) {
            co_await tx.commit();
        }
    }

    coroutine::Task<void> RowIterator::advance_partition() {
        bool               own_tx = !this->pager->transaction_active;
        pager::Transaction tx{this->pager};
        if (own_tx) {
            co_await tx.begin();
        }
        co_await this->partition_it.advance();
        auto end_it = btree::end<schema::PartitionEntry>(table->btree);
        if (partition_it != end_it) {
            auto entry = *partition_it;
            co_await setup_clustering_for_partition(*this, pager, entry);
        }
        if (own_tx) {
            co_await tx.commit();
        }
    }

    static coroutine::Task<void> create_clustering(RowIterator& it, Pager* pager, schema::Table* table) {
        if (schema::has_clustering_keys(*table) && it.partition_it != btree::end<schema::PartitionEntry>(table->btree)) {
            auto entry = *it.partition_it;
            co_await setup_clustering_for_partition(it, pager, entry);
        }
    }

    coroutine::Task<void> apply_ck_bounds_on_clustering(RowIterator& it) {
        if (!schema::has_clustering_keys(*it.table)) {
            co_return;
        }
        if (it.partition_it == btree::end<schema::PartitionEntry>(it.table->btree)) {
            co_return;
        }
        bool               own_tx = !it.pager->transaction_active;
        pager::Transaction tx{it.pager};
        if (own_tx) {
            co_await tx.begin();
        }
        auto entry = *it.partition_it;
        co_await setup_clustering_for_partition(it, it.pager, entry);
        if (own_tx) {
            co_await tx.commit();
        }
    }

    coroutine::Task<void> advance_past_empty_ck_partitions(RowIterator& it, const RowIterator& stop) {
        if (!schema::has_clustering_keys(*it.table)) {
            co_return;
        }
        auto part_end = btree::end<schema::PartitionEntry>(it.table->btree);
        while (it.partition_it != part_end && it.partition_it != stop.partition_it && !it.static_only_row && it.clustering_it == it.clustering_end_it) {
            bool               own_tx = !it.pager->transaction_active;
            pager::Transaction tx{it.pager};
            if (own_tx) {
                co_await tx.begin();
            }
            co_await it.partition_it.advance();
            if (it.partition_it != part_end) {
                auto entry = *it.partition_it;
                co_await setup_clustering_for_partition(it, it.pager, entry);
            }
            if (own_tx) {
                co_await tx.commit();
            }
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
            bool               own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) {
                co_await tx.begin();
            }
            it.partition_it = co_await btree::begin<schema::PartitionEntry>(table->btree);
            co_await create_clustering(it, pager, table);
            if (own_tx) {
                co_await tx.commit();
            }
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_eq_it(Pager* pager, schema::Table* table, S64 pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool               own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) {
                co_await tx.begin();
            }
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::RequireEquality>(table->btree, pk_key);
            co_await create_clustering(it, pager, table);
            if (own_tx) {
                co_await tx.commit();
            }
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_lt_it(Pager* pager, schema::Table* table, S64 pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool               own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) {
                co_await tx.begin();
            }
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_key);
            if (own_tx) {
                co_await tx.commit();
            }
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_le_it(Pager* pager, schema::Table* table, S64 pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool               own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) {
                co_await tx.begin();
            }
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreater>(table->btree, pk_key);
            if (own_tx) {
                co_await tx.commit();
            }
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_gt_it(Pager* pager, schema::Table* table, S64 pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool               own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) {
                co_await tx.begin();
            }
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreater>(table->btree, pk_key);
            co_await create_clustering(it, pager, table);
            if (own_tx) {
                co_await tx.commit();
            }
        }
        co_return it;
    }

    coroutine::Task<RowIterator> create_table_ge_it(Pager* pager, schema::Table* table, S64 pk_key) {
        RowIterator it;
        it.pager = pager;
        it.table = table;
        {
            bool               own_tx = !pager->transaction_active;
            pager::Transaction tx{pager};
            if (own_tx) {
                co_await tx.begin();
            }
            it.partition_it = co_await btree::find_it<schema::PartitionEntry, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_key);
            co_await create_clustering(it, pager, table);
            if (own_tx) {
                co_await tx.commit();
            }
        }
        co_return it;
    }
}
