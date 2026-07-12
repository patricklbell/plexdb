module cql.engine.clustering_compare;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;

import cql.engine.types;
import cql.engine.column_value;
import cql.engine.statements;
import cql.engine.io.codec;

using namespace plexdb;

namespace cql::clustering_compare {
    Ordering ClusteringKeyComparator::operator()(TArrayView<const U8, U16> a, TArrayView<const U8, U16> b) const noexcept {
        U16 pos_a = 0, pos_b = 0;
        for (U64 i = 0; i < this->columns.length; i++) {
            if (pos_a >= a.length || pos_b >= b.length) {
                break;
            }
            type::Basic dtype = this->columns[i].dtype;
            ColumnValue va    = io::read_column_value_sync(a.ptr, a.length, &pos_a, dtype);
            ColumnValue vb    = io::read_column_value_sync(b.ptr, b.length, &pos_b, dtype);
            S32         cmp   = compare_column_value(va, vb, dtype);
            if (this->columns[i].direction == Sort::DESC) {
                cmp = -cmp;
            }
            if (cmp < 0) {
                return Ordering::Less;
            }
            if (cmp > 0) {
                return Ordering::Greater;
            }
        }
        return Ordering::Equal;
    }
}
