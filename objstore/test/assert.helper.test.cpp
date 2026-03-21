#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <csetjmp>

import plexdb.base;

// ============================================================================
// Graceful assert recovery for Catch2 with -fno-exceptions.
//
// FAIL() calls std::terminate when exceptions are disabled. Instead we:
//   1. Record the failure with FAIL_CHECK (non-throwing).
//   2. If PLEXDB_TEST_SCOPE() was used, longjmp back to exit the test case.
// ============================================================================

namespace {

struct AssertRecovery {
    std::jmp_buf buf;
    bool         active = false;
    const char*  msg    = nullptr;
    const char*  file   = nullptr;
    const char*  func   = nullptr;
    unsigned     line   = 0;
};

thread_local AssertRecovery g_recovery;

void catch2_assert_handler(const char* msg, const char* file_name,
                           const char* function_name, unsigned line_number) {
    if (g_recovery.active) {
        g_recovery.msg  = msg;
        g_recovery.file = file_name;
        g_recovery.func = function_name;
        g_recovery.line = line_number;
        g_recovery.active = false;
        std::longjmp(g_recovery.buf, 1);
    }
    FAIL_CHECK("Assert failed \"" << msg << "\" at " << function_name << " in " << file_name << ":" << line_number);
}

struct AssertListener : Catch::EventListenerBase {
    using EventListenerBase::EventListenerBase;
    void testCaseStarting(Catch::TestCaseInfo const&) override { g_recovery.active = false; }
    void testCaseEnded(Catch::TestCaseStats const&) override   { g_recovery.active = false; }
};
CATCH_REGISTER_LISTENER(AssertListener)

struct AssertHandlerInstaller {
    AssertHandlerInstaller() { plexdb::set_assert_handler(&catch2_assert_handler); }
};
static AssertHandlerInstaller g_assert_install;

}

// Usage: place PLEXDB_TEST_SCOPE() at the top of a TEST_CASE or SECTION body.
// If a plexdb assertion fires, the failure is recorded and the scope returns.
#define PLEXDB_TEST_SCOPE()                                                     \
    do {                                                                        \
        g_recovery.active = true;                                               \
        if (setjmp(g_recovery.buf) != 0) {                                      \
            FAIL_CHECK("Assert failed \"" << g_recovery.msg << "\" at "         \
                       << g_recovery.func << " in " << g_recovery.file << ":"   \
                       << g_recovery.line);                                     \
            return;                                                             \
        }                                                                       \
    } while(0)