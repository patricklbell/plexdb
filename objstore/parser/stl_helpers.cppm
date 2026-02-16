module;
#include <string.h>

export module objstore.parser.stl_helpers;

import plexdb.base;
import plexdb.os;

using namespace plexdb;

export namespace objstore::parser {
    template<typename T, typename Size = U64>
    struct STLDynamicArray : DynamicArray<T,Size> {
        void push_back(const T& value) {
            plexdb::push_back(*this, value);
        }
    };

    template<typename T, U64 N>
    struct STLArray : Array<T,N> {
        U64 count = 0;

        void push_back(const T& value) {
            // @todo
            if (count >= N) {
                return;
            }

            this->values[this->count] = value;
            this->count++;
        }
    };

    // @todo either smarter allocation or eliminate entirely
    struct STLString : AutoString8 {
        void push_back(const char& c) {
            char* new_c_str = reinterpret_cast<char*>(os::allocate(this->length + 2));
            os::memory_copy(new_c_str, this->c_str, this->length);
            os::deallocate(this->c_str);

            this->c_str = new_c_str;
            this->c_str[this->length] = c;
            this->length++;
            this->c_str[this->length] = '\0';
        }

        void append(const char* first, const char* last) {
            assert_true(first && last && last >= first, "invalid string append arguments");

            U64 append_len = last - first;
            char* new_c_str = reinterpret_cast<char*>(os::allocate(this->length + append_len + 1));

            os::memory_copy(new_c_str, this->c_str, this->length);
            os::memory_copy(new_c_str + this->length, first, append_len);

            new_c_str[this->length + append_len] = '\0';

            os::deallocate(this->c_str);
            this->c_str = new_c_str;
            this->length += append_len;
        }
    };
}