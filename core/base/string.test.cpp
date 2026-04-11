#include <catch2/catch_test_macros.hpp>

import plexdb.base;

using namespace plexdb;

TEST_CASE("fmt_raw does not write null terminator", "[plexdb.string]") {
    SECTION("fills buffer exactly without null") {
        // Buffer filled with sentinel value
        char buf[10];
        for (int i = 0; i < 10; i++) buf[i] = 'X';
        
        String8 view{buf, 4};  // Only 4 bytes available
        fmt_raw(view, "%d", 1234);  // "1234" = 4 chars
        
        REQUIRE(view.length == 4);
        REQUIRE(buf[0] == '1');
        REQUIRE(buf[1] == '2');
        REQUIRE(buf[2] == '3');
        REQUIRE(buf[3] == '4');
        // Byte after should NOT be overwritten with null
        REQUIRE(buf[4] == 'X');
    }

    SECTION("partial buffer fill without null") {
        char buf[10];
        for (int i = 0; i < 10; i++) buf[i] = 'X';
        
        String8 view{buf, 10};
        fmt_raw(view, "%d", 42);  // "42" = 2 chars
        
        REQUIRE(view.length == 2);
        REQUIRE(buf[0] == '4');
        REQUIRE(buf[1] == '2');
        // Byte after should NOT be overwritten with null
        REQUIRE(buf[2] == 'X');
    }

    SECTION("hex format without null") {
        char buf[10];
        for (int i = 0; i < 10; i++) buf[i] = 'Y';
        
        String8 view{buf, 6};
        fmt_raw(view, "%x", 0xABCD);  // "abcd" = 4 chars
        
        REQUIRE(view.length == 4);
        REQUIRE(buf[4] == 'Y');  // Not overwritten
    }

    SECTION("string format without null") {
        char buf[20];
        for (int i = 0; i < 20; i++) buf[i] = 'Z';
        
        String8 view{buf, 20};
        fmt_raw(view, "%s", "hello");  // "hello" = 5 chars
        
        REQUIRE(view.length == 5);
        REQUIRE(buf[0] == 'h');
        REQUIRE(buf[4] == 'o');
        REQUIRE(buf[5] == 'Z');  // Not overwritten with null
    }
}

TEST_CASE("fmt_length returns correct length", "[plexdb.string]") {
    REQUIRE(fmt_length("%d", 123) == 3);
    REQUIRE(fmt_length("%d", -1) == 2);
    REQUIRE(fmt_length("%s", "hello") == 5);
    REQUIRE(fmt_length("%x", 0xFF) == 2);
    REQUIRE(fmt_length("%x", 0xFFFF) == 4);
}

TEST_CASE("append_fmt writes directly to BufferedString8", "[plexdb.string][append_fmt]") {
    SECTION("basic integer format") {
        char buf[32];
        int flush_count = 0;
        String8 flushed_data{};
        
        auto flush_fn = [&flush_count, &flushed_data](TArrayView<char>& buffer, U64 length) {
            flush_count++;
            flushed_data = String8{buffer.ptr, length};
        };
        
        {
            BufferedString8 bstr{TArrayView<char>{buf, 32}, flush_fn};
            append_fmt(bstr, "%d", 12345);
            REQUIRE(bstr.length == 5);
            REQUIRE(buf[0] == '1');
            REQUIRE(buf[4] == '5');
        }
        // Destructor flushes
        REQUIRE(flush_count == 1);
        REQUIRE(flushed_data.length == 5);
    }

    SECTION("hex format") {
        char buf[32];
        int flush_count = 0;
        
        auto flush_fn = [&flush_count](TArrayView<char>&, U64) {
            flush_count++;
        };
        
        {
            BufferedString8 bstr{TArrayView<char>{buf, 32}, flush_fn};
            append_fmt(bstr, "%x", 0xDEAD);
            REQUIRE(bstr.length == 4);
            REQUIRE(buf[0] == 'd');
            REQUIRE(buf[1] == 'e');
            REQUIRE(buf[2] == 'a');
            REQUIRE(buf[3] == 'd');
        }
        REQUIRE(flush_count == 1);
    }

    SECTION("format with existing content") {
        char buf[32];
        for (int i = 0; i < 32; i++) buf[i] = 'X';
        
        auto flush_fn = [](TArrayView<char>&, U64) {};
        
        {
            BufferedString8 bstr{TArrayView<char>{buf, 32}, flush_fn};
            append(bstr, "num=");
            append_fmt(bstr, "%d", 42);
            REQUIRE(bstr.length == 6);  // "num=42"
        }
        
        REQUIRE(buf[0] == 'n');
        REQUIRE(buf[3] == '=');
        REQUIRE(buf[4] == '4');
        REQUIRE(buf[5] == '2');
        REQUIRE(buf[6] == 'X');  // Not overwritten
    }

    SECTION("triggers flush when buffer fills") {
        char buf[8];
        int flush_count = 0;
        U64 total_flushed = 0;
        
        auto flush_fn = [&flush_count, &total_flushed](TArrayView<char>&, U64 length) {
            flush_count++;
            total_flushed += length;
        };
        
        {
            BufferedString8 bstr{TArrayView<char>{buf, 8}, flush_fn};
            // Write more than buffer size to trigger flush
            append_fmt(bstr, "%s", "hello world!");  // 12 chars > 8 buf
        }
        
        // Should have flushed at least once during write, plus destructor
        REQUIRE(total_flushed == 12);
    }

    SECTION("multiple format specifiers") {
        char buf[64];
        auto flush_fn = [](TArrayView<char>&, U64) {};
        
        {
            BufferedString8 bstr{TArrayView<char>{buf, 64}, flush_fn};
            append_fmt(bstr, "%s=%d", "value", 123);
            REQUIRE(bstr.length == 9);  // "value=123"
        }
        
        REQUIRE(String8{buf, 9} == "value=123");
    }
}
