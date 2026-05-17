export module plexdb.blob.in_memory;

import plexdb.base;
import plexdb.dynamic.containers;

using namespace plexdb;

export namespace plexdb::blob {
    struct BlobInMemory {
        U64 size_bytes = 0;
        DynamicArray<U8> data;
    };
}
