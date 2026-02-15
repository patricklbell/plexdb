export module objstore.dtypes;

import plexdb.base;

using namespace plexdb;

export namespace objstore {
    enum class DType : U8 {
        Text,
        Int,
        // Timestamp,
        // Boolean,
        // Float,
        // Double,
    };
    
    namespace dtypes {
        // @todo check validity
        using Text = plexdb::String8;
        using Int = plexdb::S64;
        // @note unix timestamp
        // using Timestamp = plexdb::U64;
        // using Boolean = bool;
        // using Float = F32;
        // using Double = F64;
    }
}
