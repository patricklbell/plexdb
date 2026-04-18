#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <stacktrace>
#include <sstream>
#include <string>

import plexdb.base;

namespace {

static std::string filter_stacktrace(const std::stacktrace& st) {
    std::ostringstream oss;

    for (std::size_t i = 0; i < st.size(); ++i) {
        auto frame = st[i];

        std::string file = frame.source_file();
        int line = frame.source_line();
        std::string desc = frame.description(); // may be empty

        // Filter out internal frames based on file path or description
        bool is_shortened = false;
        if (
            // file.find("catch2") != std::string::npos ||
            file.find("/bits/") != std::string::npos ||
            file.find("/include/c++/") != std::string::npos
        ) {
            is_shortened = true;
        }

        if (
            // desc.find("CATCH2_INTERNAL") != std::string::npos ||
            desc.find("assert_true") != std::string::npos
        ) {
            is_shortened = true;
        }

        oss << "#" << i;
        if (is_shortened) {
            oss << " [filtered]";
        } else {
            oss << " " << (desc.empty() ? "[unknown]" : desc);
        }
        oss << " at " << (file.empty() ? "??" : file) << ":" << line << "\n";
    }

    return oss.str();
}

void catch2_assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    FAIL(
        "Stack trace for assert\n" << filter_stacktrace(std::stacktrace::current()) << "\n" <<
        "Assert failed \"" << msg << "\" at " << function_name << " in " << file_name << ":" << line_number
    );
}

struct AssertHandlerInstaller {
    AssertHandlerInstaller() {
        plexdb::set_assert_handler(&catch2_assert_handler);
    }
};
static AssertHandlerInstaller g_assert_install;

} // namespace