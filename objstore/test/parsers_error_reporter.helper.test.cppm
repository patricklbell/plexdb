module;
#include <catch2/catch_message.hpp>
#include <string_view>

export module objstore.test.parsers_error_reporter;

export void catch2_cql_parse_error(const char* msg, size_t len) {
    UNSCOPED_INFO((std::string_view{msg, len}));
}
