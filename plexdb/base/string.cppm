module;
#include <string.h>
#include <stdio.h>

export module plexdb.base.string;

import plexdb.base.types;
import plexdb.base.math;

export namespace plexdb {
    struct AutoString8;

    struct String8 {
        const char* data;
        U64 length;

        constexpr String8() = default;

        String8(const char* in_buffer, U64 in_length) : data(in_buffer), length(in_length) {}

        String8(const U8* in_buffer, U64 in_length) : data(reinterpret_cast<const char*>(in_buffer)), length(in_length) {}

        template<U64 N>
        constexpr String8(const char (&lit)[N]) : data(lit), length(N - 1) {}

        template<typename Length>
        constexpr String8(const TArrayView<char,Length>& view) : data(view.ptr), length(static_cast<U64>(view.length)) {}

        template<typename Length>
        constexpr String8(const CappedTArrayView<char,Length>& view) : data(view.ptr), length(static_cast<U64>(view.cap)) {}

        String8(const char* in_c_str);
        String8(const AutoString8& str);

        constexpr operator const char*() const { return this->data; }

        const char* c_str() const;
    };

    bool operator==(const String8& a, const String8& b);
    bool operator==(const char* a, const String8& b);

    struct AutoString8 {
        char* c_str;
        U64 length;

        AutoString8();
        explicit AutoString8(U64 length);
        explicit AutoString8(const U8* in, U64 length);
        explicit AutoString8(const char* str);
        explicit AutoString8(const String8& str);

        ~AutoString8();
        AutoString8(const AutoString8& other);
        AutoString8(AutoString8&& other);

        AutoString8& operator=(const AutoString8& other) noexcept;
        AutoString8& operator=(const String8& other) noexcept;

        AutoString8& operator=(AutoString8&& other) noexcept;

        AutoString8& operator+=(const String8& rhs);
        
        operator const char*() const; 

        bool operator==(const String8& b) const;
        bool operator==(const char* b) const;

        // stl helpers
        void push_back(const char& c);
        void append(const char* first, const char* last);
    };

    void resize(AutoString8& str, U64 size);
    void push_back(AutoString8& str, const char& c);
    void append(AutoString8& str, const char* first, const char* last);
    void append(AutoString8& str, String8 s);
    
    AutoString8 operator+(const String8& lhs, const String8& rhs);   
    AutoString8 operator""_as(const char* str, size_t len);

    AutoString8 to_str(S64 x);
    AutoString8 to_str(U64 x);
    AutoString8 to_str(S32 x);
    AutoString8 to_str(U32 x);
    AutoString8 to_str(S16 x);
    AutoString8 to_str(U16 x);
    AutoString8 to_str(S8 x);
    AutoString8 to_str(U8 x);

    AutoString8 to_str(F32 x);
    AutoString8 to_str(F64 x);

    AutoString8 to_str(bool x);

    template <typename... Args>
    AutoString8 fmt(const char* fmt, Args&&... args) {
        constexpr int BUFFER_SIZE = 1024;
        char buffer[BUFFER_SIZE];

        int length = snprintf(nullptr, 0, fmt, forward<Args>(args)...);
        assert_true(length >= 0, "snprintf formatting error");
        assert_true(length < BUFFER_SIZE, "fmt result too large");
        
        length = snprintf(buffer, length+1, fmt, forward<Args>(args)...);
        assert_true(length >= 0, "snprintf formatting error");

        return AutoString8(reinterpret_cast<U8*>(buffer), length);
    }

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

    template<typename F>
    concept BufferedString8Flush = requires(F f, const char* data, U64 len) {
        f(data, len);
    };

    template<BufferedString8Flush F>
    struct BufferedString8 {
        TArrayView<char> buffer;
        U64 length;
        F flush;

        BufferedString8(TArrayView<char> buf, F flush_fn) : buffer(buf), length(0), flush(flush_fn) {}
    };
    
    template<BufferedString8Flush F>
    void flush(BufferedString8<F>& str) {
        if (str.length >= str.buffer.length) {
            str.flush(str.buffer.ptr, str.length);
            str.length = 0;
        }
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, char c) {
        flush(str);

        assert_true(str.length < str.buffer.length, "string8 buffer is zero length");
        str.buffer.ptr[str.length++] = c;
    }
    
    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, String8 prefix) {
        const char* src = prefix.data;
        const char* end = prefix.data + prefix.length;

        while (src != end) {
            flush(str);
            
            U64 count = min(static_cast<U64>(end - src), str.buffer.length - str.length);
            assert_true(count > 0, "string8 buffer is zero length");
            
            // @todo memory copy
            for (U64 i = 0; i < count; i++) {
                str.buffer.ptr[str.length++] = src[i];
            }

            src += count;
        }
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, const char* prefix) {
        append(str, String8(prefix));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, S64 value) {
        char tmp[32];
        int len = snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(value));
        assert_true(len > 0 && len < 32, "format error");

        append(str, String8(tmp, static_cast<U64>(len)));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, S32 value) {
        char tmp[16];
        int len = snprintf(tmp, sizeof(tmp), "%d", value);
        assert_true(len > 0 && len < 16, "format error");

        append(str, String8(tmp, static_cast<U64>(len)));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, S16 value) {
        char tmp[8];
        int len = snprintf(tmp, sizeof(tmp), "%d", static_cast<int>(value));
        assert_true(len > 0 && len < 8, "format error");

        append(str, String8(tmp, static_cast<U64>(len)));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, U8 value) {
        char tmp[4];
        int len = snprintf(tmp, sizeof(tmp), "%u", static_cast<unsigned>(value));
        assert_true(len > 0 && len < 4, "format error");

        append(str, String8(tmp, static_cast<U64>(len)));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, F64 value) {
        char tmp[32];
        int len = snprintf(tmp, sizeof(tmp), "%g", value);
        assert_true(len > 0 && len < 32, "format error");

        append(str, String8(tmp, static_cast<U64>(len)));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, F32 value) {
        char tmp[32];
        int len = snprintf(tmp, sizeof(tmp), "%g", static_cast<double>(value));
        assert_true(len > 0 && len < 32, "format error");

        append(str, String8(tmp, static_cast<U64>(len)));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, bool value) {
        append(str, value ? String8("true") : String8("false"));
    }
}