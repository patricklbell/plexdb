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

export namespace cql::key {
    // @note clustering-key bytes are not memcmp-orderable; always compare via this.
    Ordering compare_clustering(const schema::Table& tbl, TArrayView<const U8, U16> a, TArrayView<const U8, U16> b);

    DynamicArray<U8> encode_clustering_single(const schema::Table& tbl, const Evaluated& val);
    DynamicArray<U8> encode_clustering(const schema::Table& tbl, TArrayView<const Evaluated, U64> clustering_vals);
    DynamicArray<U8> encode_clustering_prefix(const schema::Table& tbl, TArrayView<const Evaluated, U64> vals);

    // @note index keys use the same self-delimiting encoding as clustering keys.
    DynamicArray<U8> encode_index_prefix(const Evaluated& val, type::Basic dtype);
    DynamicArray<U8> encode_index_prefix_from_cv(const ColumnValue& cv, type::Basic dtype);

    // Prefix for `WHERE m[k] = v`: key component then value component.
    DynamicArray<U8> encode_index_prefix_entries(const Evaluated& key_val, type::Basic key_dtype, const Evaluated& val_val, type::Basic val_dtype);

    // Token component followed by the partition-key columns (tiebreak on token collision).
    DynamicArray<U8> encode_index_pk_component(const schema::Table& tbl, S64 pk_token, TArrayView<const ColumnValue, U64> row_values);

    // @note layout: indexed-value component(s) ++ pk component ++ ck_bytes.
    DynamicArray<U8> make_full_index_key(const DynamicArray<U8>& prefix, TArrayView<const U8, U16> pk_bytes, TArrayView<const U8, U16> ck_bytes);

    U16 index_key_ck_start(const schema::Table& tbl, const schema::Index& idx, TArrayView<const U8, U16> key_view);

    S64 compute_partition_token(const schema::Table& tbl, TArrayView<const ColumnValue, U64> row_values);
    S64 compute_partition_token_from_evals(const schema::Table& tbl, TArrayView<const Evaluated, U64> evals);

    // @note partial CK prefixes are legal; a component is only absent past the last one present.
    DynamicArray<ColumnValue> decode_clustering(const schema::Table& tbl, TArrayView<const U8, U16> key_bytes);
}
