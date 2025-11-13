module plexdb.base;

namespace plexdb {
    void set_assert_handler(AssertHandler h) noexcept {
        g_assert_handler = h;
    }
}