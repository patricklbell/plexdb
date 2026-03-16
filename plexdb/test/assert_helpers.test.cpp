#include <catch2/catch_test_macros.hpp>

import plexdb.base;

inline void catch2_assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    FAIL_CHECK("Assert failed \"" << msg << "\" at " << function_name << " in " << file_name << ":" << line_number);
}

struct AssertHandlerInstaller {
    AssertHandlerInstaller() {
        plexdb::set_assert_handler(&catch2_assert_handler);
    }
};

static AssertHandlerInstaller install;