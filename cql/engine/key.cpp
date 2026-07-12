module cql.engine.key;

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
    static void append_component(DynamicArray<U8>& out, const ColumnValue& cv, type::Basic dtype) {
        assert_true(dtype != type::Basic::duration, "duration cannot be used as a clustering/index key component");
        auto       sink = io::sync_buffer_writer(out);
        io::Writer w    = io::to_writer(sink);
        io::write_column_value(w, cv, type::Type{dtype});
    }

    static ColumnValue eval_to_cv(const Evaluated& eval, type::Basic dtype) {
        if (type_matches_tag<ColumnValue>(eval.value)) {
            return get<ColumnValue>(eval.value);
        }
        Optional<ColumnValue> resolved = io::resolve_literal_scalar(get<Literal>(eval.value), type::Type{dtype});
        assert_true(resolved.has_value(), "clustering/index key value must already be a ColumnValue for this type");
        return move(*resolved);
    }

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

    Ordering compare_clustering(const schema::Table& tbl, TArrayView<const U8, U16> a, TArrayView<const U8, U16> b) {
        return schema::make_clustering_key_policy(tbl).comparator(a, b);
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

    DynamicArray<U8> encode_clustering_single(const schema::Table& tbl, const Evaluated& val) {
        assert_true(tbl.clustering_key_col_indices.length >= 1, "table has no clustering key");
        return encode_clustering_prefix(tbl, {&val, 1});
    }

    DynamicArray<U8> encode_clustering(const schema::Table& tbl, TArrayView<const Evaluated, U64> clustering_vals) {
        assert_true(clustering_vals.length == tbl.clustering_key_col_indices.length, "clustering val count must match clustering key column count");
        return encode_clustering_prefix(tbl, clustering_vals);
    }

    DynamicArray<U8> encode_index_prefix_from_cv(const ColumnValue& cv, type::Basic dtype) {
        DynamicArray<U8> out;
        append_component(out, cv, dtype);
        return out;
    }

    DynamicArray<U8> encode_index_prefix(const Evaluated& val, type::Basic dtype) {
        return encode_index_prefix_from_cv(eval_to_cv(val, dtype), dtype);
    }

    DynamicArray<U8> encode_index_prefix_entries(const Evaluated& key_val, type::Basic key_dtype, const Evaluated& val_val, type::Basic val_dtype) {
        DynamicArray<U8> out;
        append_component(out, eval_to_cv(key_val, key_dtype), key_dtype);
        append_component(out, eval_to_cv(val_val, val_dtype), val_dtype);
        return out;
    }

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

    DynamicArray<U8> make_full_index_key(const DynamicArray<U8>& prefix, TArrayView<const U8, U16> pk_bytes, TArrayView<const U8, U16> ck_bytes) {
        DynamicArray<U8> out;
        auto             sink = io::sync_buffer_writer(out);
        sink(prefix.ptr, prefix.length);
        sink(pk_bytes.ptr, pk_bytes.length);
        if (ck_bytes.length > 0) {
            sink(ck_bytes.ptr, ck_bytes.length);
        }
        return out;
    }

    U16 index_key_ck_start(const schema::Table& tbl, const schema::Index& idx, TArrayView<const U8, U16> key_view) {
        U64 ck_count   = tbl.clustering_key_col_indices.length;
        U64 skip_count = idx.key_specs.length - ck_count;
        U16 pos        = 0;
        for (U64 i = 0; i < skip_count; i++) {
            io::read_column_value_sync(key_view.ptr, key_view.length, &pos, idx.key_specs[i].dtype);
        }
        return pos;
    }

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
