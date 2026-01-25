module;
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

module plexdb.base.string;

import plexdb.os;

namespace plexdb {
    String8::String8(const AutoString8& str) : data(str.data), length(str.length) {}

    AutoString8::AutoString8()
        : data(nullptr), length(0) {}
    AutoString8::AutoString8(const U8* in, U64 length)
        : length(length) {
        this->data = os::allocate(this->length+1);
        os::memory_copy(this->data, in, this->length);
        this->data[this->length] = '\0';
    }
    AutoString8::AutoString8(const char* str)
        : AutoString8(reinterpret_cast<const U8*>(str), strlen(str)) {}
    AutoString8::AutoString8(const String8& str)
        : AutoString8(reinterpret_cast<const U8*>(str.data), str.length) {}
    AutoString8::~AutoString8() {
        if (this->data)
            os::deallocate(this->data);
    }
    AutoString8::AutoString8(AutoString8&& other) noexcept
        : data(exchange(other.data, nullptr)), length(exchange(other.length, 0)) {}

    AutoString8& AutoString8::operator=(const AutoString8& other) {
        return *this = AutoString8(other.data, other.length);
    }
    AutoString8& AutoString8::operator=(AutoString8&& other) noexcept {
        swap(this->data, other.data);
        swap(this->length, other.length);
        return *this;
    }

    AutoString8& AutoString8::operator+=(const AutoString8& rhs) {
        U64 new_length = this->length + rhs.length;
        U8* new_data = os::allocate(new_length + 1);

        os::memory_copy(new_data, this->data, this->length);
        os::deallocate(this->data);
        os::memory_copy(new_data + this->length, rhs.data, rhs.length);

        this->data = new_data;
        this->length = new_length;
        this->data[this->length] = '\0';
        return *this;
    }
    AutoString8 operator+(const AutoString8& lhs, const AutoString8& rhs) {
        U64 length = lhs.length + rhs.length;
        U8* data = os::allocate(length + 1);
        os::memory_copy(data, lhs.data, lhs.length);
        os::memory_copy(data + lhs.length, rhs.data, rhs.length);

        AutoString8 res{};
        res.data = data;
        res.length = length;
        return res;
    }
    AutoString8::operator const char*() const {
        return this->c_str;
    }
    bool AutoString8::operator==(const AutoString8& b) const {
        return strcmp(this->c_str, b.c_str) == 0;
    }
    bool AutoString8::operator==(const char* b) const {
        return strcmp(this->c_str, b) == 0;
    }

    AutoString8 to_string8(S64 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIi64, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIi64, x);
        return res;
    }
    AutoString8 to_string8(U64 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIu64, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIu64, x);
        return res;
    }
    AutoString8 to_string8(S32 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIi32, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIi32, x);
        return res;
    }
    AutoString8 to_string8(U32 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIu32, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIu32, x);
        return res;
    }
    AutoString8 to_string8(S16 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIi16, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIi16, x);
        return res;
    }
    AutoString8 to_string8(U16 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIu16, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIu16, x);
        return res;
    }
    AutoString8 to_string8(S8 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIi8, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIi8, x);
        return res;
    }
    AutoString8 to_string8(U8 x) {
        AutoString8 res{};
        res.length = snprintf(nullptr, 0, "%" PRIu8, x);
        res.data = os::allocate(res.length+1);
        res.length = snprintf((char*)res.data, res.length+1, "%" PRIu8, x);
        return res;
    }

    void print(const String8& str) {
        if (str.length == 0)
            return;
        printf("%s", str.c_str);
    }
}