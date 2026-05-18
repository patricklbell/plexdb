#include <coroutine>
import plexdb.coroutine;
import plexdb.pager.test_helpers;

#define PLEXDB_PAGER_TEST_CAT2(a, b) a##b
#define PLEXDB_PAGER_TEST_CAT(a, b) PLEXDB_PAGER_TEST_CAT2(a, b)
// Wraps a Catch2 test as a coroutine driven by drive_test_pager. Allows co_await
// inside test bodies while keeping SECTION/GENERATE support (drive is synchronous
// so Catch2's section-tracking re-invocations work identically to regular functions).
#define PAGER_TEST_CASE(name, tags) \
    static coroutine::Task<void> PLEXDB_PAGER_TEST_CAT(pager_test_, __LINE__)(); \
    TEST_CASE(name, tags) { drive_test_pager(PLEXDB_PAGER_TEST_CAT(pager_test_, __LINE__)()); } \
    static coroutine::Task<void> PLEXDB_PAGER_TEST_CAT(pager_test_, __LINE__)()
