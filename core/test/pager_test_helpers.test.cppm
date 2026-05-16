module;
#include <coroutine>

export module plexdb.pager.test_helpers;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.pager;
import plexdb.pager.wal;
import plexdb.pager.types;
import plexdb.coroutine;

using namespace plexdb;

export {
    inline auto g_test_sync_file_io_ctx = aio::create_sync_file_io_context();
    inline aio::EventConsumer g_test_sync_consumer{0, aio::OnUnblockFunctor{[](const TArrayView<os::PollEvent>&) -> bool { return true; }}};
    inline os::Poll g_test_poll{};

    inline Pager test_pager(os::Handle f) {
        Pager p;
        aio::drive(pager::init(p, &g_test_sync_file_io_ctx, f), g_test_sync_consumer, g_test_poll);
        return p;
    }
    inline Pager test_pager(os::Handle f, os::Handle wal) {
        Pager p;
        aio::drive(pager::init(p, &g_test_sync_file_io_ctx, f, wal), g_test_sync_consumer, g_test_poll);
        return p;
    }

    inline Pager create_test_pager(os::Handle f, U64 page_size) {
        pager::Header header = aio::drive(pager::create(g_test_sync_file_io_ctx, f, page_size), g_test_sync_consumer, g_test_poll);
        return Pager{&g_test_sync_file_io_ctx, f, header};
    }
    inline Pager create_test_pager(os::Handle f, os::Handle wal, U64 page_size) {
        Pager p;
        pager::Header header = aio::drive(pager::create(g_test_sync_file_io_ctx, f, page_size), g_test_sync_consumer, g_test_poll);
        aio::drive(pager::init(p, &g_test_sync_file_io_ctx, f, wal, header), g_test_sync_consumer, g_test_poll);
        return p;
    }

    inline void destroy_test_pager(Pager& p) {
        aio::drive(pager::destroy(p), g_test_sync_consumer, g_test_poll);
    }

    inline Wal create_test_wal(os::Handle f, U64 page_size) {
        return Wal{f, aio::drive(wal::create(g_test_sync_file_io_ctx, f, page_size), g_test_sync_consumer, g_test_poll)};
    }


    template<typename T>
    T drive_test_pager(coroutine::Task<T> task) {
        pump_task(task, g_test_sync_consumer, g_test_poll);
        return plexdb::move(task.value());
    }
    inline void drive_test_pager(coroutine::Task<void> task) {
        pump_task(task, g_test_sync_consumer, g_test_poll);
    }
}
