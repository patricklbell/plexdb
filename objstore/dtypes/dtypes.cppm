export module objstore.dtypes;

import plexdb.base;
import plexdb.tagged_union;

using namespace plexdb;

export namespace objstore {
    enum class DType : U8 {
        text,
        int_,
        bigint,
        smallint,
        counter,
        timestamp,
        boolean,
        float_,
        double_,
        uuid,
    };
    
    namespace dtypes {
        // @todo check validity
        using Text = plexdb::String8;
        using Int = plexdb::S32;
        using Bigint = plexdb::S64;
        using Smallint = plexdb::S16;
        using Counter = plexdb::S64;
        using Timestamp = plexdb::TaggedUnion<plexdb::String8, plexdb::S64>;
        using Boolean = bool;
        using Float = F32;
        using Double = F64;
        using Uuid = plexdb::String8;
    }
}
