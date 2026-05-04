module;
#include <profiling/tracy.hpp>

module objstore.engine.it;

import plexdb.base;
import plexdb.pager;
import plexdb.btree;
import plexdb.btree.types;
import plexdb.blob;

import objstore.engine.io;
import objstore.engine.schema;
import objstore.engine.statements;

namespace objstore {
    // ========================================================================
    // column iterator
    //   @todo move mask logic into io
    // ========================================================================
    ColumnIterator::ColumnIterator() {
        this->table = nullptr;
    }

    static U64 get_row_column_count(blob::BlobDynamicPaged& blob) {
        U64 column_count = 0;
        static_assert(sizeof(column_count) == io::COLUMN_COUNT_BYTE_COUNT);
        blob::get(blob, reinterpret_cast<U8*>(&column_count), sizeof(column_count));
        return column_count;
    }

    static void get_column_current_mask(U64* out_mask, blob::BlobDynamicPaged& blob, U64 new_column_idx) {
        if (new_column_idx % io::MASK_BIT_COUNT == 0) {
            static_assert(sizeof(*out_mask) == io::MASK_BYTE_COUNT, "mask out size matches io size");
            blob::get(blob, reinterpret_cast<U8*>(out_mask), sizeof(*out_mask),
                      io::COLUMN_COUNT_BYTE_COUNT + io::MASK_BYTE_COUNT * (new_column_idx / io::MASK_BIT_COUNT));
        }
    }

    ColumnIterator::ColumnIterator(Pager* pager, const schema::Table* table, U64 page_idx) {
        this->table = table;
        assert_true(this->table->cols.length != 0, "column cannot be empty, it must at least have a PK");

        this->row_blob = blob::BlobDynamicPaged{pager, page_idx};
        this->row_column_count = get_row_column_count(this->row_blob);
        this->current_column_idx = 0;
        // header layout: [column_count(U64)] + [ceil(row_column_count/64) * mask(U64)]
        this->current_byte_offset = io::COLUMN_COUNT_BYTE_COUNT +
            ceil_div(this->row_column_count, io::MASK_BIT_COUNT) * io::MASK_BYTE_COUNT;
        get_column_current_mask(&this->current_mask, this->row_blob, this->current_column_idx);
    }

    ColumnValue ColumnIterator::operator*() { ZoneScopedN("it::column_read");
        assert_true(this->table != nullptr, "cannot dereference an end iterator, this should never happen!");

        if (this->current_column_idx >= this->row_column_count) {
            return {Null{}};
        }
        {
            U64 bit_idx = this->current_column_idx % io::MASK_BIT_COUNT;
            if (!(this->current_mask & (1_u64 << bit_idx))) {
                return {Null{}};
            }
        }

        U64 read_offset = this->current_byte_offset;
        return io::read_column_value(
            [this, &read_offset](U8* out_value, U64 size) {
                blob::get(this->row_blob, out_value, size, read_offset);
                read_offset += size;
            },
            this->table->cols[this->current_column_idx].type
        );
    }

    ColumnIterator& ColumnIterator::operator++() {
        assert_true(this->table != nullptr, "cannot increment end iterator");
        if (this->current_column_idx >= this->row_column_count) {
            this->table = nullptr;
            return *this;
        }
        assert_true(this->current_byte_offset <= this->row_blob.size_bytes,
                    "sum of column sizes did not match expectation from blob size");

        // advance byte offset past current column's stored bytes (only if active)
        U64 bit_idx = this->current_column_idx % io::MASK_BIT_COUNT;
        if (this->current_mask & (1_u64 << bit_idx)) {
            io::read_column_value(
                [this](U8* out_value, U64 size) {
                    // @todo @perf IMPORTANT! avoid this read (accounting for variable length types)
                    blob::get(this->row_blob, out_value, size, this->current_byte_offset);
                    this->current_byte_offset += size;
                },
                this->table->cols[this->current_column_idx].type
            );
        }

        this->current_column_idx++;
        get_column_current_mask(&this->current_mask, this->row_blob, this->current_column_idx);

        return *this;
    }

    // ========================================================================
    // row iterator
    // ========================================================================
    ColumnRange RowIterator::operator*() {
        assert_true(*this != create_table_end_it(this->pager, this->table),
                    "cannot dereference an end iterator, this should never happen!");

        return {
            .start = ColumnIterator{this->pager, this->table, *this->pk_hash_it},
            .stop = ColumnIterator{},
        };
    }

    RowIterator& RowIterator::operator++() {
        ++this->pk_hash_it;
        return *this;
    }

    RowIterator create_table_begin_it(Pager* pager, schema::Table* table) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tbegin<U64>(table->btree),
        };
    }

    RowIterator create_table_end_it(Pager* pager, schema::Table* table) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tend<U64>(table->btree),
        };
    }

    RowIterator create_table_eq_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tfind_it<U64, btree::SearchStrategy::RequireEquality>(table->btree, pk_hash),
        };
    }

    // stop iterator: exclusive end for "< pk_hash" → position at first key >= pk_hash
    RowIterator create_table_lt_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tfind_it<U64, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_hash),
        };
    }

    // stop iterator: exclusive end for "<= pk_hash" → position at first key > pk_hash
    RowIterator create_table_le_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tfind_it<U64, btree::SearchStrategy::FirstGreater>(table->btree, pk_hash),
        };
    }

    // start iterator: first key > pk_hash
    RowIterator create_table_gt_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tfind_it<U64, btree::SearchStrategy::FirstGreater>(table->btree, pk_hash),
        };
    }

    // start iterator: first key >= pk_hash
    RowIterator create_table_ge_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        return RowIterator{
            .pager = pager,
            .table = table,
            .pk_hash_it = btree::tfind_it<U64, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_hash),
        };
    }
}
