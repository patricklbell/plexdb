module;
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdlib.h>

#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

module plexdb.base.string;

import plexdb.os;
import xxhash;

namespace plexdb {
    AutoString8 bytes_to_hex(const U8* data, U64 len) {
        constexpr char hx[] = "0123456789abcdef";
        AutoString8    result{len * 2};
        for (U64 i = 0; i < len; i++) {
            result.c_str[i * 2]     = hx[data[i] >> 4];
            result.c_str[i * 2 + 1] = hx[data[i] & 0xf];
        }
        return result;
    }

    String8::String8(const AutoString8& str)
        : data(str.c_str)
        , length(str.length) {
    }
    String8::String8(const char* in_c_str)
        : data(in_c_str)
        , length(strlen(in_c_str)) {
    }

    const char* String8::c_str() const {
        assert_true(strlen(this->data) == this->length, "c_str called on non-null terminated string");
        return this->data;
    }

    bool operator==(const String8& a, const String8& b) {
        if (a.length != b.length) {
            return false;
        }
        for (U64 i = 0; i < b.length; i++) {
            if (a.data[i] != b.data[i]) {
                return false;
            }
        }
        return true;
    }
    bool operator==(const char* a, const String8& b) {
        U64 i = 0;
        for (; a[i] != '\0' && i < b.length; i++) {
            if (a[i] != b.data[i]) {
                return false;
            }
        }
        return i == b.length;
    }

    Optional<U64> find(const String8& str, char c) {
        for (U64 i = 0; i < str.length; i++) {
            if (str.data[i] == c) {
                return {i};
            }
        }
        return {};
    }

    bool contains(String8 haystack, String8 needle) {
        if (needle.length > haystack.length) {
            return false;
        }
        for (U64 i = 0; i <= haystack.length - needle.length; i++) {
            bool match = true;
            for (U64 j = 0; j < needle.length; j++) {
                if (haystack.data[i + j] != needle.data[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    }

    AutoString8::AutoString8()
        : c_str(nullptr)
        , length(0) {
    }
    AutoString8::AutoString8(U64 length)
        : length(length) {
        this->c_str               = reinterpret_cast<char*>(os::allocate(this->length + 1));
        this->c_str[this->length] = '\0';
    }
    AutoString8::AutoString8(const U8* in, U64 length)
        : AutoString8(length) {
        os::memory_copy(this->c_str, in, this->length);
    }
    AutoString8::AutoString8(const char* str)
        : AutoString8(reinterpret_cast<const U8*>(str), ::strlen(str)) {
    }
    AutoString8::AutoString8(const String8& str)
        : AutoString8(reinterpret_cast<const U8*>(str.data), str.length) {
    }
    AutoString8::AutoString8(char c, U64 length)
        : AutoString8(length) {
        os::memory_set(this->c_str, c, this->length);
    }

    AutoString8::~AutoString8() {
        if (this->c_str) {
            os::deallocate(this->c_str);
        }
    }
    AutoString8::AutoString8(const AutoString8& other)
        : AutoString8(reinterpret_cast<U8*>(other.c_str), other.length) {
    }
    AutoString8::AutoString8(AutoString8&& other)
        : c_str(exchange(other.c_str, nullptr))
        , length(exchange(other.length, 0)) {
    }

    AutoString8& AutoString8::operator=(const AutoString8& other) noexcept {
        return *this = AutoString8(reinterpret_cast<const U8*>(other.c_str), other.length);
    }
    AutoString8& AutoString8::operator=(const String8& other) noexcept {
        return *this = AutoString8(reinterpret_cast<const U8*>(other.data), other.length);
    }
    AutoString8& AutoString8::operator=(AutoString8&& other) noexcept {
        swap(this->c_str, other.c_str);
        swap(this->length, other.length);
        return *this;
    }

    AutoString8& AutoString8::operator+=(const String8& rhs) {
        U64   new_length = this->length + rhs.length;
        char* new_c_str  = reinterpret_cast<char*>(os::allocate(new_length + 1));

        os::memory_copy(new_c_str, this->c_str, this->length);
        os::deallocate(this->c_str);
        os::memory_copy(new_c_str + this->length, rhs.data, rhs.length);

        this->c_str               = new_c_str;
        this->length              = new_length;
        this->c_str[this->length] = '\0';
        return *this;
    }

    AutoString8::operator const char*() const {
        return this->c_str;
    }

    bool AutoString8::operator==(const String8& b) const {
        if (this->length != b.length) {
            return false;
        }

        for (U64 i = 0; i < b.length; i++) {
            if (b.data[i] != this->c_str[i]) {
                return false;
            }
        }
        return true;
    }
    bool AutoString8::operator==(const char* b) const {
        return strcmp(this->c_str, b) == 0;
    }
    bool AutoString8::operator==(const AutoString8& b) const {
        if (this->length != b.length) {
            return false;
        }
        for (U64 i = 0; i < this->length; i++) {
            if (this->c_str[i] != b.c_str[i]) {
                return false;
            }
        }
        return true;
    }
    U64 hash(const AutoString8& s) {
        U64 result = XXHash64::hash(s.c_str, s.length, 0);
        return result | (result == 0);
    }

    U64 hash(String8 s) {
        U64 result = XXHash64::hash(s.data, s.length, 0);
        return result | (result == 0);
    }

    void resize(AutoString8& str, U64 length) {
        if (length < str.length) {
            str.length        = length;
            str.c_str[length] = '\0';
            return;
        }
        if (length == str.length) {
            return;
        }

        char* new_c_str = reinterpret_cast<char*>(os::allocate(length + 1));
        os::memory_copy(new_c_str, str.c_str, str.length);
        os::deallocate(str.c_str);
        str.c_str         = new_c_str;
        str.length        = length;
        str.c_str[length] = '\0';
    }

    void push_back(AutoString8& str, const char& c) {
        char* new_c_str = reinterpret_cast<char*>(os::allocate(str.length + 2));
        os::memory_copy(new_c_str, str.c_str, str.length);
        os::deallocate(str.c_str);

        str.c_str             = new_c_str;
        str.c_str[str.length] = c;
        str.length++;
        str.c_str[str.length] = '\0';
    }

    void append(AutoString8& str, const char* first, const char* last) {
        assert_true(first && last && last >= first, "invalid string append arguments");

        U64   append_len = last - first;
        char* new_c_str  = reinterpret_cast<char*>(os::allocate(str.length + append_len + 1));

        os::memory_copy(new_c_str, str.c_str, str.length);
        os::memory_copy(new_c_str + str.length, first, append_len);

        new_c_str[str.length + append_len] = '\0';

        os::deallocate(str.c_str);
        str.c_str = new_c_str;
        str.length += append_len;
    }

    void append(AutoString8& str, String8 s) {
        append(str, s.data, s.data + s.length);
    }

    void to_lowercase_inplace(AutoString8& str) {
        for (U64 i = 0; i < str.length; i++) {
            char c       = str.c_str[i];
            str.c_str[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
    }

    AutoString8 operator+(const String8& lhs, const String8& rhs) {
        U64   length = lhs.length + rhs.length;
        char* c_str  = reinterpret_cast<char*>(os::allocate(length + 1));
        os::memory_copy(c_str, lhs.data, lhs.length);
        os::memory_copy(c_str + lhs.length, rhs.data, rhs.length);
        c_str[length] = '\0';

        AutoString8 res{};
        res.c_str  = c_str;
        res.length = length;
        return res;
    }
    AutoString8 operator""_as(const char* str, size_t len) {
        return AutoString8(reinterpret_cast<const U8*>(str), len);
    }

    AutoString8 to_str(const String8& x) {
        return AutoString8{x};
    }

    AutoString8 to_str(const AutoString8& x) {
        return x;
    }

    AutoString8 to_str(S64 x) {
        return fmt("%" PRIi64, x);
    }
    AutoString8 to_str(U64 x) {
        return fmt("%" PRIu64, x);
    }
    AutoString8 to_str(S32 x) {
        return fmt("%" PRIi32, x);
    }
    AutoString8 to_str(U32 x) {
        return fmt("%" PRIu32, x);
    }
    AutoString8 to_str(S16 x) {
        return fmt("%" PRIi16, x);
    }
    AutoString8 to_str(U16 x) {
        return fmt("%" PRIu16, x);
    }
    AutoString8 to_str(S8 x) {
        return fmt("%" PRIi8, x);
    }
    AutoString8 to_str(U8 x) {
        return fmt("%" PRIu8, x);
    }

    AutoString8 to_str(F32 x) {
        return fmt("%f", x);
    }
    AutoString8 to_str(F64 x) {
        return fmt("%lf", x);
    }

    AutoString8 to_str(bool x) {
        return AutoString8((x) ? "true" : "false");
    }

    // @note these functions copy to ensure null-termination since String8 may not be null-terminated
    S64 s64_from_str(String8 x) {
        AutoString8 tmp(x);
        return strtoll(tmp.c_str, nullptr, 10);
    }

    U64 u64_from_str(String8 x) {
        AutoString8 tmp(x);
        return strtoull(tmp.c_str, nullptr, 10);
    }

    S32 s32_from_str(String8 x) {
        AutoString8 tmp(x);
        return static_cast<S32>(strtol(tmp.c_str, nullptr, 10));
    }

    U32 u32_from_str(String8 x) {
        AutoString8 tmp(x);
        return static_cast<U32>(strtoul(tmp.c_str, nullptr, 10));
    }

    S16 s16_from_str(String8 x) {
        AutoString8 tmp(x);
        return static_cast<S16>(strtol(tmp.c_str, nullptr, 10));
    }

    U16 u16_from_str(String8 x) {
        AutoString8 tmp(x);
        return static_cast<U16>(strtoul(tmp.c_str, nullptr, 10));
    }

    S8 s8_from_str(String8 x) {
        AutoString8 tmp(x);
        return static_cast<S8>(strtol(tmp.c_str, nullptr, 10));
    }

    U8 u8_from_str(String8 x) {
        AutoString8 tmp(x);
        return static_cast<U8>(strtoul(tmp.c_str, nullptr, 10));
    }

    F32 f32_from_str(String8 x) {
        AutoString8 tmp(x);
        return strtof(tmp.c_str, nullptr);
    }

    F64 f64_from_str(String8 x) {
        AutoString8 tmp(x);
        return strtod(tmp.c_str, nullptr);
    }

    bool bool_from_str(String8 x) {
        return x == "true" || x == "1";
    }

    // @todo simplify the callbacks
    namespace {
        struct FmtContext {
            char* buf;
            U64   remaining;
            U64   written;
        };

        char* fmt_callback(const char*, void* user, int len) {
            auto* ctx = static_cast<FmtContext*>(user);
            ctx->written += len;
            if (static_cast<U64>(len) > ctx->remaining) {
                ctx->remaining = 0;
                return nullptr;
            }
            ctx->buf += len;
            ctx->remaining -= len;
            return ctx->buf;
        }

        struct AppendFmtContext {
            char* buf_ptr;
            U64   buf_len;
            U64*  length_ptr;
            void* flush_ctx;
            void (*flush_fn)(void* ctx, const char* data, U64 len);
        };

        char* append_fmt_callback(const char* buf, void* user, int len) {
            auto* ctx = static_cast<AppendFmtContext*>(user);

            const char* src       = buf;
            int         remaining = len;

            while (remaining > 0) {
                U64 space = ctx->buf_len - *ctx->length_ptr;

                if (space == 0) {
                    ctx->flush_fn(ctx->flush_ctx, ctx->buf_ptr, *ctx->length_ptr);
                    *ctx->length_ptr = 0;
                    space            = ctx->buf_len;
                }

                U64 to_copy = (static_cast<U64>(remaining) < space) ? remaining : space;
                os::memory_copy(ctx->buf_ptr + *ctx->length_ptr, src, to_copy);
                *ctx->length_ptr += to_copy;
                src += to_copy;
                remaining -= to_copy;
            }

            return const_cast<char*>(buf);
        }
    }

    U64 fmt_raw_impl(char* buf, U64 buf_len, const char* fmt, ...) {
        FmtContext ctx{buf, buf_len, 0};
        va_list    va;
        va_start(va, fmt);
        stbsp_vsprintfcb(fmt_callback, &ctx, ctx.buf, fmt, va);
        va_end(va);
        return ctx.written;
    }

    void append_fmt_impl(char* buf_ptr, U64 buf_len, U64* length_ptr, void* flush_ctx, void (*flush_fn)(void*, const char*, U64), const char* fmt, ...) {
        char             working_buf[STB_SPRINTF_MIN];
        AppendFmtContext ctx{buf_ptr, buf_len, length_ptr, flush_ctx, flush_fn};
        va_list          va;
        va_start(va, fmt);
        stbsp_vsprintfcb(append_fmt_callback, &ctx, working_buf, fmt, va);
        va_end(va);
    }

    void print(const String8& str) {
        if (str.length == 0) {
            return;
        }
        printf("%.*s", (int)str.length, str.data);
    }
}
