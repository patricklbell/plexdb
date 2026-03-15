#include <catch2/catch_message.hpp>

#include "parsers_error_reporter.hpp"

// Catch2 error reporter for CQL parse errors.
// Routes error messages through UNSCOPED_INFO so they are displayed
// on test failure without requiring captures in every call site.
void catch2_cql_parse_error(const char* msg, size_t len) {
    UNSCOPED_INFO((std::string_view{msg, len}));
}
