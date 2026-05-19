module;
#include <coroutine>

module plexdb.btree.paged;

import plexdb.coroutine;
import plexdb.pager;
import plexdb.btree.node;
import plexdb.os;

namespace plexdb::btree {


    // Explicit instantiations
    template struct BTreePaged<FixedKeyPolicy<U64>, FixedValuePolicy>;
    template coroutine::Task<U64> create_paged(Pager&, FixedKeyPolicy<U64>, FixedValuePolicy);
    template struct BTreePaged<VarlenKeyPolicy<>, VarlenValuePolicy>;
    template coroutine::Task<U64> create_paged(Pager&, VarlenKeyPolicy<>, VarlenValuePolicy);
}
