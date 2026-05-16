export module keyvalue.engine;

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.containers;

export import keyvalue.engine.statements;
import keyvalue.store;

using namespace plexdb;

export namespace keyvalue::engine {
    struct ResultNull {};
    struct ResultEmptyArr {};
    struct ResultClose {};
    struct ResultSimpleStr {
        String8 value;
    };
    struct ResultBulkStr {
        AutoString8 value;
    };
    struct ResultInt {
        S64 value;
    };
    struct ResultBulkArr {
        DynamicArray<AutoString8> values;
    };
    struct ResultNullBulkArr {
        DynamicArray<Optional<AutoString8>> values;
    };
    struct ResultScan {
        AutoString8 cursor; DynamicArray<AutoString8> keys;
    };
    struct ResultError {
        AutoString8 message;
    };

    using ExecutionResult = TaggedUnion<
        ResultSimpleStr, ResultNull, ResultEmptyArr,
        ResultBulkStr, ResultInt,
        ResultBulkArr, ResultNullBulkArr,
        ResultScan, ResultError, ResultClose
    >;

    ExecutionResult execute(store::Store& store, const Statement& statement);
}
