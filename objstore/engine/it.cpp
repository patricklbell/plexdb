module;
#include <coroutine>
#include <profiling/tracy.hpp>

module objstore.engine.it;

import plexdb.base;
import plexdb.coroutine;
import plexdb.os;
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
    coroutine::Task<ColumnIterator> ColumnIterator::load(Pager* pager, const schema::Table* table, U64 page_idx) {
        ColumnIterator it{};
        it.table = table;
        assert_true(it.table->cols.length != 0, "column cannot be empty, it must at least have a PK");

        blob::BlobDynamicPaged row_blob = co_await blob::BlobDynamicPaged::load(pager, page_idx);

        resize(it.row_data, row_blob.size_bytes);
        co_await blob::get(row_blob, it.row_data.ptr, row_blob.size_bytes);

        static_assert(sizeof(it.row_column_count) == io::COLUMN_COUNT_BYTE_COUNT);
        os::memory_copy(&it.row_column_count, it.row_data.ptr, sizeof(it.row_column_count));

        it.current_column_idx = 0;
        // header layout: [column_count(U64)] + [ceil(row_column_count/64) * mask(U64)]
        it.current_byte_offset = io::COLUMN_COUNT_BYTE_COUNT +
            ceil_div(it.row_column_count, io::MASK_BIT_COUNT) * io::MASK_BYTE_COUNT;

        if (it.row_column_count > 0) {
            static_assert(sizeof(it.current_mask) == io::MASK_BYTE_COUNT, "mask size matches io size");
            os::memory_copy(&it.current_mask, it.row_data.ptr + io::COLUMN_COUNT_BYTE_COUNT, sizeof(it.current_mask));
        }

        co_return move(it);
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
                os::memory_copy(out_value, this->row_data.ptr + read_offset, size);
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
        assert_true(this->current_byte_offset <= this->row_data.length,
                    "sum of column sizes did not match expectation from row data size");

        // advance byte offset past current column's stored bytes (only if active)
        U64 bit_idx = this->current_column_idx % io::MASK_BIT_COUNT;
        if (this->current_mask & (1_u64 << bit_idx)) {
            io::read_column_value(
                [this](U8* out_value, U64 size) {
                    // @todo @perf avoid copy for variable-length skip (track byte size separately)
                    os::memory_copy(out_value, this->row_data.ptr + this->current_byte_offset, size);
                    this->current_byte_offset += size;
                },
                this->table->cols[this->current_column_idx].type
            );
        }

        this->current_column_idx++;

        if (this->current_column_idx < this->row_column_count &&
            this->current_column_idx % io::MASK_BIT_COUNT == 0) {
            U64 mask_offset = io::COLUMN_COUNT_BYTE_COUNT +
                io::MASK_BYTE_COUNT * (this->current_column_idx / io::MASK_BIT_COUNT);
            os::memory_copy(&this->current_mask, this->row_data.ptr + mask_offset, sizeof(this->current_mask));
        }

        return *this;
    }

    // ========================================================================
    // row iterator
    // ========================================================================
    coroutine::Task<ColumnRange> RowIterator::deref() {
        ColumnIterator col_it = co_await ColumnIterator::load(this->pager, this->table, *this->pk_hash_it);
        co_return ColumnRange{
            .start = move(col_it),
            .stop  = ColumnIterator{},
        };
    }

    coroutine::Task<void> RowIterator::advance() {
        co_await this->pk_hash_it.advance();
    }

    RowIterator create_table_end_it(Pager* pager, schema::Table* table) {
        return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = btree::tend<U64>(table->btree),
        };
    }

    coroutine::Task<RowIterator> create_table_begin_it(Pager* pager, schema::Table* table) {
        auto pk_hash_it = co_await btree::tbegin<U64>(table->btree);
        co_return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = move(pk_hash_it),
        };
    }

    coroutine::Task<RowIterator> create_table_eq_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        auto pk_hash_it = co_await btree::tfind_it<U64, btree::SearchStrategy::RequireEquality>(table->btree, pk_hash);
        co_return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = move(pk_hash_it),
        };
    }

    // stop iterator: exclusive end for "< pk_hash" → position at first key >= pk_hash
    coroutine::Task<RowIterator> create_table_lt_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        auto pk_hash_it = co_await btree::tfind_it<U64, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_hash);
        co_return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = move(pk_hash_it),
        };
    }

    // stop iterator: exclusive end for "<= pk_hash" → position at first key > pk_hash
    coroutine::Task<RowIterator> create_table_le_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        auto pk_hash_it = co_await btree::tfind_it<U64, btree::SearchStrategy::FirstGreater>(table->btree, pk_hash);
        co_return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = move(pk_hash_it),
        };
    }

    // start iterator: first key > pk_hash
    coroutine::Task<RowIterator> create_table_gt_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        auto pk_hash_it = co_await btree::tfind_it<U64, btree::SearchStrategy::FirstGreater>(table->btree, pk_hash);
        co_return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = move(pk_hash_it),
        };
    }

    // start iterator: first key >= pk_hash
    coroutine::Task<RowIterator> create_table_ge_it(Pager* pager, schema::Table* table, U64 pk_hash) {
        auto pk_hash_it = co_await btree::tfind_it<U64, btree::SearchStrategy::FirstGreaterEqual>(table->btree, pk_hash);
        co_return RowIterator{
            .pager      = pager,
            .table      = table,
            .pk_hash_it = move(pk_hash_it),
        };
    }
}
