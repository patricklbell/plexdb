#include <catch2/catch_test_macros.hpp>

import plexdb.base;
import plexdb.dynamic.containers;

import keyvalue.resp.protocol;

using namespace plexdb;

using namespace keyvalue;

TEST_CASE("RESP protocol: parse round-trip", "[keyvalue.resp.protocol]") {
    using namespace keyvalue::resp::protocol;

    DynamicArray<U8> buf;

    append_simple_string(buf, "OK");
    append_error(buf, "ERR", "msg");
    append_integer(buf, 42);
    append_bulk_string(buf, "hello");
    append_null_bulk_string(buf);
    append_array_header(buf, 3);

    bool found_plus = false, found_minus = false, found_colon = false;
    bool found_dollar = false, found_star = false;
    for (U64 i = 0; i < buf.length; i++) {
        switch (buf[i]) {
            case '+':
                found_plus = true;
                break;
            case '-':
                found_minus = true;
                break;
            case ':':
                found_colon = true;
                break;
            case '$':
                found_dollar = true;
                break;
            case '*':
                found_star = true;
                break;
        }
    }
    CHECK(found_plus);
    CHECK(found_minus);
    CHECK(found_colon);
    CHECK(found_dollar);
    CHECK(found_star);
}
