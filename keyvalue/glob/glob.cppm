export module keyvalue.glob;

import plexdb.base;

using namespace plexdb;

export namespace keyvalue::glob {
    bool match(String8 key, String8 pattern);
}
