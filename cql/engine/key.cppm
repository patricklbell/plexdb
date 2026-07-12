export module cql.engine.key;

import plexdb.base;
import plexdb.os;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.io.codec;
import cql.engine.io.evaluator;
import cql.engine.schema;
import cql.engine.token;
import cql.engine.types;
import cql.engine.statements;

using namespace plexdb;

namespace cql::key {
    static void append_bytes(DynamicArray<U8>& out, const U8* src, U16 len) {
        U64 old_len = out.length;
        resize(out, old_len + len);
        os::memory_copy(out.ptr + old_len, src, len);
    }

    // Appends one self-delimiting component to a growing clustering/index-key byte string —
    // the generic io.codec column-value wire shape, same as row/static storage, just with no
    // header/mask around it. Every key.cppm encoder shares this instead of each hand-rolling
    // the Writer-sink setup.
    static void append_component(DynamicArray<U8>& out, const ColumnValue& cv, type::Basic dtype) {
        assert_true(dtype != type::Basic::duration, "duration cannot be used as a clustering/index key component");
        auto       sink = io::sync_buffer_writer(out);
        io::Writer w    = io::to_writer(sink);
        io::write_column_value(w, cv, type::Type{dtype});
    }

    // Widen an Evaluated to the ColumnValue shape append_component expects. A resolved
    // ColumnValue passes through as-is; a bare Literal is resolved via
    // io::resolve_literal_scalar (io.evaluator) — the same Literal->ColumnValue coercion the
    // storage write path uses, so this can't drift from it.
    static ColumnValue eval_to_cv(const Evaluated& eval, type::Basic dtype) {
        if (type_matches_tag<ColumnValue>(eval.value)) {
            return get<ColumnValue>(eval.value);
        }
        Optional<ColumnValue> resolved = io::resolve_literal_scalar(get<Literal>(eval.value), type::Type{dtype});
        assert_true(resolved.has_value(), "clustering/index key value must already be a ColumnValue for this type");
        return move(*resolved);
    }
}

namespace cql::key {
    // Hashes an already-resolved partition key straight into a Murmur3 token without
    // materializing the intermediate wire bytes: the component encoder writes directly into
    // the incremental hash state via a Writer sink.
    template<typename ComponentFn>
    static S64 hash_partition_components(ComponentFn&& emit_components) {
        token::Murmur3State state{};
        auto                sink = [&state](const U8* p, U64 n) {
            state.update(p, n);
        };
        io::Writer w = io::to_writer(sink);
        emit_components(w);
        return state.finalize();
    }
}

export namespace cql::key {
    // The only correct way to order two clustering-key byte strings — they are not
    // memcmp-orderable (see ClusteringKeyComparator). Any code outside the ClusteringBTree
    // itself that needs to compare or bound clustering keys must go through this.
    Ordering compare_clustering(const schema::Table& tbl, TArrayView<const U8, U16> a, TArrayView<const U8, U16> b) {
        return schema::make_clustering_key_policy(tbl).comparator(a, b);
    }

    // Serialize a partition key to its BTree key: the raw S64 Murmur3 token (the BTree's
    // FixedKeyPolicy<S64> compares it directly via operator<, matching Cassandra's signed
    // token ordering — no byte encoding needed). Raw partition-key column values live in
    // the partition's static-storage preamble instead (see engine.cpp's rewrite_static).
    S64 compute_partition_token_from_eval(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.partition_key_col_indices.length >= 1, "table has no partition key");
        U64                   col_idx = tbl.partition_key_col_indices[0];
        const schema::Column& col     = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "partition key must be a basic type");
        type::Basic dtype = get<type::Basic>(col.type.value);
        return hash_partition_components([&](io::Writer w) {
            token::write_component(w, eval_to_cv(val, dtype), dtype, false);
        });
    }

    // Serialize the first clustering key column as a range-bound prefix.
    DynamicArray<U8> encode_clustering_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.clustering_key_col_indices.length >= 1, "table has no clustering key");
        U64                   col_idx = tbl.clustering_key_col_indices[0];
        const schema::Column& col     = tbl.cols[col_idx];
        assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
        type::Basic      dtype = get<type::Basic>(col.type.value);
        DynamicArray<U8> out;
        append_component(out, eval_to_cv(val, dtype), dtype);
        return out;
    }

    // Serialize clustering key bytes from a parallel array of evaluated values.
    DynamicArray<U8> encode_clustering(const schema::Table& tbl, TArrayView<const Evaluated, U64> clustering_vals) {
        assert_true(clustering_vals.length == tbl.clustering_key_col_indices.length, "clustering val count must match clustering key column count");
        DynamicArray<U8> out;
        for (U64 i = 0; i < tbl.clustering_key_col_indices.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            type::Basic dtype = get<type::Basic>(col.type.value);
            append_component(out, eval_to_cv(clustering_vals[i], dtype), dtype);
        }
        return out;
    }

    DynamicArray<U8> encode_clustering_prefix(const schema::Table& tbl, TArrayView<const Evaluated, U64> vals) {
        assert_true(vals.length <= tbl.clustering_key_col_indices.length, "prefix length exceeds clustering key column count");
        DynamicArray<U8> out;
        for (U64 i = 0; i < vals.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            assert_true(type_matches_tag<type::Basic>(col.type.value), "clustering key must be a basic type");
            type::Basic dtype = get<type::Basic>(col.type.value);
            append_component(out, eval_to_cv(vals[i], dtype), dtype);
        }
        return out;
    }

    // Index-key search/maintenance components use the same self-delimiting typed encoding
    // (append_component, i.e. io.codec's column-value wire shape) as clustering keys, not a
    // separate order-preserving codec — IndexBTree's comparator decodes them via
    // Index::key_specs (schema::make_index_key_policy) exactly like ClusteringBTree decodes
    // clustering keys.
    DynamicArray<U8> encode_index_prefix(const Evaluated& val, type::Basic dtype) {
        DynamicArray<U8> out;
        append_component(out, eval_to_cv(val, dtype), dtype);
        return out;
    }

    DynamicArray<U8> encode_index_prefix_from_cv(const ColumnValue& cv, type::Basic dtype) {
        DynamicArray<U8> out;
        append_component(out, cv, dtype);
        return out;
    }

    // Entries-index lookup prefix for `WHERE m[k] = v` (and maintenance writes): key
    // component followed by value component, each independently self-delimiting.
    DynamicArray<U8> encode_index_prefix_entries(const Evaluated& key_val, type::Basic key_dtype, const Evaluated& val_val, type::Basic val_dtype) {
        DynamicArray<U8> out;
        append_component(out, eval_to_cv(key_val, key_dtype), key_dtype);
        append_component(out, eval_to_cv(val_val, val_dtype), val_dtype);
        return out;
    }

    // Index-key pk payload: the embedded partition token (as a bigint component) followed by
    // the table's partition-key columns (tiebreak on the vanishingly rare token collision).
    // Distinct from the partition BTree key itself, which is just the bare S64 token (see
    // compute_partition_token_from_evals) — this is what an index entry carries to both order entries
    // sharing an indexed value (primarily by token, matching Cassandra's per-partition 2i
    // ordering) and jump back to tbl.btree after a hit.
    DynamicArray<U8> encode_index_pk_component(const schema::Table& tbl, S64 pk_token, TArrayView<const ColumnValue, U64> row_values) {
        DynamicArray<U8> out;
        append_component(out, ColumnValue{pk_token}, type::Basic::bigint);
        for (U64 pk_ci : tbl.partition_key_col_indices) {
            assert_true(pk_ci < row_values.length, "partition column index out of range");
            type::Basic dtype = get<type::Basic>(tbl.cols[pk_ci].type.value);
            append_component(out, row_values[pk_ci], dtype);
        }
        return out;
    }

    // @note index key layout: indexed-value component(s) ++ pk component
    // (encode_index_pk_component: token then pk columns) ++ ck_bytes. Every component is
    // self-delimiting, so plain concatenation is enough — no length markers needed between
    // them (see index_key_ck_start for the inverse).
    DynamicArray<U8> make_full_index_key(const DynamicArray<U8>& prefix, TArrayView<const U8, U16> pk_bytes, TArrayView<const U8, U16> ck_bytes) {
        DynamicArray<U8> out;
        append_bytes(out, prefix.ptr, static_cast<U16>(prefix.length));
        append_bytes(out, pk_bytes.ptr, pk_bytes.length);
        if (ck_bytes.length > 0) {
            append_bytes(out, ck_bytes.ptr, ck_bytes.length);
        }
        return out;
    }

    // Offset into an index key where ck_bytes begins (== key length if the table has no
    // clustering key). Skips the indexed-value component(s), the embedded token, and the
    // partition-key columns — none of their decoded values are needed here, since an index
    // hit's IndexEntry value already carries the row's static_page/row_page directly.
    U16 index_key_ck_start(const schema::Table& tbl, const schema::Index& idx, TArrayView<const U8, U16> key_view) {
        U64 ck_count   = tbl.clustering_key_col_indices.length;
        U64 skip_count = idx.key_specs.length - ck_count;
        U16 pos        = 0;
        for (U64 i = 0; i < skip_count; i++) {
            io::read_column_value_sync(key_view.ptr, key_view.length, &pos, idx.key_specs[i].dtype);
        }
        return pos;
    }

    // Compute the Murmur3 partition token from a row's column values, hashing
    // the Cassandra wire-format bytes directly without building the full
    // lex-encoded key.
    S64 compute_partition_token(const schema::Table& tbl, TArrayView<const ColumnValue, U64> row_values) {
        bool composite = tbl.partition_key_col_indices.length > 1;
        return hash_partition_components([&](io::Writer w) {
            for (U64 pk_ci : tbl.partition_key_col_indices) {
                assert_true(pk_ci < row_values.length, "partition column index out of range");
                type::Basic dtype = get<type::Basic>(tbl.cols[pk_ci].type.value);
                token::write_component(w, row_values[pk_ci], dtype, composite);
            }
        });
    }

    // Same token computation, but taking already-evaluated argument values (the
    // shape produced by the term registry when token(...) appears in a term
    // context). evals[i] is encoded as the type of partition_key_col_indices[i].
    S64 compute_partition_token_from_evals(const schema::Table& tbl, TArrayView<const Evaluated, U64> evals) {
        assert_true(evals.length == tbl.partition_key_col_indices.length, "token(...) arity must match partition key column count");
        bool composite = tbl.partition_key_col_indices.length > 1;
        return hash_partition_components([&](io::Writer w) {
            for (U64 i = 0; i < evals.length; i++) {
                type::Basic dtype = get<type::Basic>(tbl.cols[tbl.partition_key_col_indices[i]].type.value);
                token::write_component(w, eval_to_cv(evals[i], dtype), dtype, composite);
            }
        });
    }

    // @note partial CK prefixes are legal; trailing positions with no bytes left are simply
    // absent. Every component (including an empty blob/text value) is length-prefixed, so
    // "no bytes left" unambiguously means "no more components".
    DynamicArray<ColumnValue> decode_clustering(const schema::Table& tbl, TArrayView<const U8, U16> key_bytes) {
        DynamicArray<ColumnValue> out;
        U16                       pos = 0;
        for (U64 i = 0; i < tbl.clustering_key_col_indices.length && pos < key_bytes.length; i++) {
            U64                   col_idx = tbl.clustering_key_col_indices[i];
            const schema::Column& col     = tbl.cols[col_idx];
            type::Basic           dtype   = get<type::Basic>(col.type.value);
            push_back(out, io::read_column_value_sync(key_bytes.ptr, key_bytes.length, &pos, dtype));
        }
        return out;
    }
}
