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

    struct AutoString8 {
        union {
            U8* data;
            const char* c_str;
        };
        U64 length;

        AutoString8();
        AutoString8(const U8* in, U64 length);
        AutoString8(const char* str);
        AutoString8(const String8& str);
        ~AutoString8();
        AutoString8(AutoString8&& other) noexcept;

        AutoString8& operator=(const AutoString8& other);
        AutoString8& operator=(AutoString8&& other) noexcept;
        AutoString8& operator+=(const AutoString8& rhs);
        friend AutoString8 operator+(const AutoString8& lhs, const AutoString8& rhs);
        operator const char*() const; 
    };

    AutoString8 to_string8(S64 x);
    AutoString8 to_string8(U64 x);
    AutoString8 to_string8(S32 x);
    AutoString8 to_string8(U32 x);
    AutoString8 to_string8(S16 x);
    AutoString8 to_string8(U16 x);
    AutoString8 to_string8(S8 x);
    AutoString8 to_string8(U8 x);
}