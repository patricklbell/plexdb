export module cql.engine.clustering_compare;

import plexdb.base;
import plexdb.os;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.types;
import cql.engine.column_value;
import cql.engine.statements;

using namespace plexdb;

// Clustering-key column ordering. Component encoding is the generic io.codec column-value
// wire shape (used directly by callers, not re-exported here) — this module owns only the
// per-column sort spec and the resulting byte-string comparator.
export namespace cql::clustering_compare {
    struct ClusteringColumnSpec {
        type::Basic dtype;
        Sort        direction;
    };

    struct ClusteringKeyComparator {
        TArrayView<const ClusteringColumnSpec, U64> columns{nullptr, 0};

        Ordering operator()(TArrayView<const U8, U16> a, TArrayView<const U8, U16> b) const noexcept;
    };
}
