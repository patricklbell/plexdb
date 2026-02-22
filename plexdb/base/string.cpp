module;
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

module plexdb.base.string;

import plexdb.os;

namespace plexdb {
    String8::String8(const AutoString8& str) : data(str.c_str), length(str.length) {}
    String8::String8(const char* in_c_str) : data(in_c_str), length(strlen(in_c_str)) {}

    const char* String8::c_str() const {
        assert_true(strlen(this->data) == this->length, "c_str called on non-null terminated string");
        return this->data;
    }

    bool operator==(const String8& a, const String8& b) {
        if (a.length != b.length)
            return false;
        for (U64 i = 0; i < b.length; i++) {
            if (a.data[i] != b.data[i])
                return false;
        }
        return true;
    }
    bool operator==(const char* a, const String8& b) {
        U64 i = 0;
        for (; a[i] != '\0' && i < b.length; i++) {
            if (a[i] != b.data[i])
                return false;
        }
        return i == b.length;
    }

    AutoString8::AutoString8()
        : c_str(nullptr), length(0) {}
    AutoString8::AutoString8(U64 length)
        : length(length) {
        this->c_str = reinterpret_cast<char*>(os::allocate(this->length+1));
        this->c_str[this->length] = '\0';
    }
    AutoString8::AutoString8(const U8* in, U64 length)
        : AutoString8(length) {
        os::memory_copy(this->c_str, in, this->length);
    }
    AutoString8::AutoString8(const char* str)
        : AutoString8(reinterpret_cast<const U8*>(str), strlen(str)) {}
    AutoString8::AutoString8(const String8& str)
        : AutoString8(reinterpret_cast<const U8*>(str.data), str.length) {}

    AutoString8::~AutoString8() {
        if (this->c_str)
            os::deallocate(this->c_str);
    }
    AutoString8::AutoString8(const AutoString8& other)
        : AutoString8(reinterpret_cast<U8*>(other.c_str), other.length) {}
    AutoString8::AutoString8(AutoString8&& other)
        : c_str(exchange(other.c_str, nullptr)), length(exchange(other.length, 0)) {}

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
        U64 new_length = this->length + rhs.length;
        char* new_c_str = reinterpret_cast<char*>(os::allocate(new_length + 1));

        os::memory_copy(new_c_str, this->c_str, this->length);
        os::deallocate(this->c_str);
        os::memory_copy(new_c_str + this->length, rhs.data, rhs.length);

        this->c_str = new_c_str;
        this->length = new_length;
        this->c_str[this->length] = '\0';
        return *this;
    }

    AutoString8::operator const char*() const {
        return this->c_str;
    }

    bool AutoString8::operator==(const String8& b) const {
        return strcmp(this->c_str, b.c_str()) == 0;
    }
    bool AutoString8::operator==(const char* b) const {
        return strcmp(this->c_str, b) == 0;
    }

    void AutoString8::push_back(const char& c) {
        plexdb::push_back(*this, c);
    }
    void AutoString8::append(const char* first, const char* last) {
        plexdb::append(*this, first, last);
    }

    void resize(AutoString8& str, U64 length) {
        if (str.length >= length) {
            return;
        }
        
        os::deallocate(str.c_str);
        
        str.length = length;
        str.c_str = reinterpret_cast<char*>(os::allocate(str.length+1));
        str.c_str[str.length] = '\0';
    }

    void push_back(AutoString8& str, const char& c) {
        char* new_c_str = reinterpret_cast<char*>(os::allocate(str.length + 2));
        os::memory_copy(new_c_str, str.c_str, str.length);
        os::deallocate(str.c_str);

        str.c_str = new_c_str;
        str.c_str[str.length] = c;
        str.length++;
        str.c_str[str.length] = '\0';
    }

    void append(AutoString8& str, const char* first, const char* last) {
        assert_true(first && last && last >= first, "invalid string append arguments");

        U64 append_len = last - first;
        char* new_c_str = reinterpret_cast<char*>(os::allocate(str.length + append_len + 1));

        os::memory_copy(new_c_str, str.c_str, str.length);
        os::memory_copy(new_c_str + str.length, first, append_len);

        new_c_str[str.length + append_len] = '\0';

        os::deallocate(str.c_str);
        str.c_str = new_c_str;
        str.length += append_len;
    }

    AutoString8 operator+(const String8& lhs, const String8& rhs) {
        U64 length = lhs.length + rhs.length;
        char* c_str = reinterpret_cast<char*>(os::allocate(length + 1));
        os::memory_copy(c_str, lhs.data, lhs.length);
        os::memory_copy(c_str + lhs.length, rhs.data, rhs.length);
        c_str[length] = '\0';

        AutoString8 res{};
        res.c_str = c_str;
        res.length = length;
        return res;
    }
    AutoString8 operator""_as(const char* str, size_t len) {
        return AutoString8(reinterpret_cast<const U8*>(str), len);
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

    void print(const String8& str) {
        if (str.length == 0)
            return;
        printf("%.*s", (int)str.length, str.data);
    }
}