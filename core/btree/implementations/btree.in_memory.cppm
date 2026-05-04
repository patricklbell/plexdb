export module plexdb.btree.in_memory;

import plexdb.base;
import plexdb.btree.types;

export namespace plexdb::btree {
    struct BTreeInMemory {
        Header header;

        explicit BTreeInMemory(CountType max_keys_per_internal, CountType max_keys_per_leaf, U64 value_stride);
        ~BTreeInMemory();

        struct Transaction {
            Transaction();
            Transaction(BTreeInMemory* t);
            Transaction(Transaction&& t);

            Transaction(const Transaction& other) = delete;
            Transaction& operator=(const Transaction& other) = delete;

            BTreeInMemory* t;
        };
    };

    BTreeInMemory::Transaction scope(const BTreeInMemory::Transaction& t);
}