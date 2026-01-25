export module plexdb.base.string;

import plexdb.base.types;

export namespace plexdb {
    struct AutoString8;

    struct String8 {
        union {
            U8* data;
            const char* c_str;
        };
        U64 length;

        template<U64 N>
        String8(const char (&lit)[N]) : c_str(lit), length(N) {}
        String8(const AutoString8& str);
    };

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
        bool operator==(const AutoString8& b) const;
        bool operator==(const char* b) const;
    };

    AutoString8 to_string8(S64 x);
    AutoString8 to_string8(U64 x);
    AutoString8 to_string8(S32 x);
    AutoString8 to_string8(U32 x);
    AutoString8 to_string8(S16 x);
    AutoString8 to_string8(U16 x);
    AutoString8 to_string8(S8 x);
    AutoString8 to_string8(U8 x);

    void print(const String8& str);

    template <typename... Args>
    void print(const String8& first, const Args&... rest)
    {
        print(first);
        (print(rest), ...);
    }

    template <typename... Args>
    void println(const Args&... args)
    {
        print(args...);
        print("\n");
    }

}