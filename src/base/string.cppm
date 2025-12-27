export module plexdb.base.string;

import plexdb.base.types;

export namespace plexdb {
    struct String8 {
        union {
            U8* data;
            const char* c_str;
        };
        U64 length;
    };

    template<U64 N>
    constexpr String8 string8(const char (&str)[N]) {
        return String8{.c_str=str, .length=N-1};
    }
}