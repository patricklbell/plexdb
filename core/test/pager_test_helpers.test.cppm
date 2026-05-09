module;
#include <coroutine>

export module plexdb.pager.test_helpers;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.pager;
import plexdb.pager.types;
import plexdb.coroutine;

using namespace plexdb;

export namespace pager_test {
    inline aio::FileReadFunctor  g_sync_read {[](os::Handle f, U64 off, U32 n, U8* dst) -> coroutine::Task<aio::Error> {
        os::file_read(f, Rng1U64{.start=off, .end=off+n}, dst);
        co_return aio::Error::None;
    }};
    inline aio::FileWriteFunctor g_sync_write{[](os::Handle f, U64 off, U32 n, const U8* src) -> coroutine::Task<aio::Error> {
        os::file_write(f, Rng1U64{.start=off, .end=off+n}, src);
        co_return aio::Error::None;
    }};
    inline aio::FileSyncFunctor  g_sync_sync {[](os::Handle f) -> coroutine::Task<aio::Error> {
        os::file_sync(f);
        co_return aio::Error::None;
    }};
    inline aio::SubmitFunctor    g_sync_submit{[]() {}};
    inline aio::DrainFunctor     g_sync_drain {[]() -> U32 { return 0; }};
    inline aio::WakeFdFunctor    g_sync_wake  {[]() -> os::Handle { return os::zero_handle(); }};
    inline aio::FileIOContext    g_sync_ctx{&g_sync_read, &g_sync_write, &g_sync_sync,
                                            &g_sync_submit, &g_sync_drain, &g_sync_wake};

    inline Pager make_pager(os::Handle f) {
        Pager p{f};
        p.file_io_ctx = &g_sync_ctx;
        return p;
    }
    inline Pager make_pager(os::Handle f, const pager::Header& hdr) {
        Pager p{f, hdr};
        p.file_io_ctx = &g_sync_ctx;
        return p;
    }
    inline Pager make_pager(os::Handle f, os::Handle wal) {
        Pager p{f, wal};
        p.file_io_ctx = &g_sync_ctx;
        return p;
    }
    inline Pager make_pager(os::Handle f, os::Handle wal, const pager::Header& hdr) {
        Pager p{f, wal, hdr};
        p.file_io_ctx = &g_sync_ctx;
        return p;
    }
} // namespace pager_test
