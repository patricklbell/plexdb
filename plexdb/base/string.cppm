module;
#include "stb_sprintf.h"

export module plexdb.base.string;

import plexdb.base.types;
import plexdb.base.math;

namespace plexdb {
    // @note writes to inout.data WITHOUT null terminator, returns bytes written
    U64 fmt_raw_impl(char* buf, U64 buf_len, const char* fmt, ...);

    // @note appends formatted output directly to BufferedString8, flushing as needed
    void append_fmt_impl(char* buf_ptr, U64 buf_len, U64* length_ptr, void* flush_ctx, void (*flush_fn)(void*, const char*, U64), const char* fmt, ...);
}

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
    U64 fmt_length(const char* fmt, Args&&... args) {
        return stbsp_snprintf(nullptr, 0, fmt, forward<Args>(args)...);
    }

    template <typename... Args>
    void fmt_raw(String8& inout, const char* fmt, Args&&... args) {
        inout.length = fmt_raw_impl(const_cast<char*>(inout.data), inout.length, fmt, forward<Args>(args)...);
    }

    template <typename... Args>
    AutoString8 fmt(const char* fmt_str, Args&&... args) {
        U64 length = fmt_length(fmt_str, forward<Args>(args)...);
        AutoString8 result(length);
        String8 view{result.c_str, length};
        fmt_raw(view, fmt_str, forward<Args>(args)...);
        return result;
    }

    void print(const String8& str);
    
    template <typename... Args>
    void print(const String8& first, const Args&... rest) {
        print(first);
        (print(rest), ...);
    }

    template <typename... Args>
    void println(const Args&... args) {
        print(args...);
        print("\n");
    }

    template<typename F>
    concept BufferedString8Flush = requires(F f, const char* data, U64 length, bool is_final) {
        f(data, length, is_final);
    };

    template<BufferedString8Flush F>
    struct BufferedString8 {
        TArrayView<char> buffer;
        U64 length;
        F flush;

        BufferedString8(TArrayView<char> buf, F flush_fn) : buffer(buf), length(0), flush(flush_fn) {}
        ~BufferedString8() { this->flush(this->buffer.ptr, this->length, /*is_final=*/true); }
    };
    
    template<BufferedString8Flush F>
    void flush(BufferedString8<F>& str) {
        assert_true(str.length <= str.buffer.length, "invalid flush state, length overflows buffer");

        if (str.length == str.buffer.length) {
            str.flush(str.buffer.ptr, str.length, /*is_final=*/false);
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
}

namespace plexdb {
    template<BufferedString8Flush F>
    void buffered_string_flush_wrapper(void* ctx, const char* data, U64 len) {
        auto* str = static_cast<BufferedString8<F>*>(ctx);
        flush(*str);
    }
}

export namespace plexdb {
    template<BufferedString8Flush F, typename... Args>
    void append_fmt(BufferedString8<F>& str, const char* fmt, Args&&... args) {
        append_fmt_impl(
            str.buffer.ptr, str.buffer.length, &str.length,
            &str, buffered_string_flush_wrapper<F>,
            fmt, forward<Args>(args)...
        );
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, S64 value) {
        append_fmt(str, "%lld", static_cast<long long>(value));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, S32 value) {
        append_fmt(str, "%d", value);
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, S16 value) {
        append_fmt(str, "%d", static_cast<int>(value));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, U8 value) {
        append_fmt(str, "%u", static_cast<unsigned>(value));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, F64 value) {
        append_fmt(str, "%g", value);
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, F32 value) {
        append_fmt(str, "%g", static_cast<double>(value));
    }

    template<BufferedString8Flush F>
    void append(BufferedString8<F>& str, bool value) {
        append(str, value ? String8("true") : String8("false"));
    }

    template <BufferedString8Flush F, typename First, typename... Rest>
    void append(BufferedString8<F>& str, const First& first, const Rest&... rest)
    {
        append(str, first);
        if constexpr (sizeof...(rest) > 0) {
            append(str, rest...);
        }
    }
}