module;
#include "macros.h"
#include <coroutine>

export module plexdb.aio;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;
import plexdb.threads;
import plexdb.os.uring;
import plexdb.arena;

using namespace plexdb;

export namespace plexdb::aio {
    // ========================================================================
    // Event loop
    // ========================================================================
    using OnUnblockFunctor = AutoFunctor<bool(const TArrayView<os::PollEvent>&)>;
    struct EventConsumer {
        U64 max_events;
        OnUnblockFunctor on_unblock;
    };

    template<typename... Cs>
    void run_blocking_event_loop(const os::Poll& unblock, Cs&... consumers) {
        U64 total_max_events = (consumers.max_events + ... + U64(0));
        if (total_max_events == 0) total_max_events = 1;

        threads::Scope scratch = threads::scratch();
        os::PollEvent* ev_storage = arena::push_array<os::PollEvent>(*scratch.arena, total_max_events);
        CappedTArrayView<os::PollEvent> events_view{ev_storage, total_max_events, 0};

        while (true) {
            events_view.cap = 0;
            os::block_until_poll_unblocks_wth_events(unblock, &events_view);
            TArrayView<os::PollEvent> events{events_view.ptr, events_view.cap};

            bool stop = false;
            auto dispatch = [&](auto& c) { stop |= !c.on_unblock(events); };
            (dispatch(consumers), ...);

            if (stop) break;
        }
    }

    EventConsumer create_notifier_consumer(os::Notifier& notifier, os::Poll& poll);

    // ========================================================================
    // File I/O
    // ========================================================================
    using FileReadFunctor  = AutoFunctor<coroutine::Task<>(os::Handle, Rng1U64 rng, void* out)>;
    using FileWriteFunctor = AutoFunctor<coroutine::Task<>(os::Handle, Rng1U64 rng, const void* in)>;
    using FileSyncFunctor  = AutoFunctor<coroutine::Task<>(os::Handle)>;

    struct FileIOContext {
        FileReadFunctor  read;
        FileWriteFunctor write;
        FileSyncFunctor  sync;
        // @note resize and stat is assumed to be fast enough synchronously
    };

    coroutine::Task<> file_read (FileIOContext& ctx, os::Handle file, Rng1U64 rng, void* out);
    coroutine::Task<> file_write(FileIOContext& ctx, os::Handle file, Rng1U64 rng, const void* in);
    coroutine::Task<> file_sync (FileIOContext& ctx, os::Handle file);

    Pair<FileIOContext, EventConsumer> create_aio_async_file_io_context(os::AIOContext* aio_ctx, arena::Arena& arena, os::Poll& poll);
    Pair<FileIOContext, EventConsumer> create_uring_async_file_io_context(uring::Ring* ring, arena::Arena& arena, os::Poll& poll);
    FileIOContext                      create_sync_file_io_context();

    // ========================================================================
    // drive
    //   Drives a coroutine to completion, pumping the event consumer on each
    //   suspension. Degrades to coroutine::drive behaviour for sync contexts
    //   (max_events == 0) since their tasks never suspend.
    // ========================================================================
    template<typename TaskT>
    void pump_task(TaskT& task, EventConsumer& consumer, os::Poll& poll) {
        U64 max = consumer.max_events > 0 ? consumer.max_events : 1;
        threads::Scope scratch = threads::scratch();
        os::PollEvent* ev_storage = arena::push_array<os::PollEvent>(*scratch.arena, max);
        CappedTArrayView<os::PollEvent> events{ev_storage, max, 0};
        task.resume();
        while (!task.done()) {
            events.cap = 0;
            os::block_until_poll_unblocks_wth_events(poll, &events);
            (void)consumer.on_unblock(TArrayView<os::PollEvent>{events.ptr, events.cap});
        }
    }

    template<typename T>
    T drive(coroutine::Task<T> task, EventConsumer& consumer, os::Poll& poll) {
        pump_task(task, consumer, poll);
        return plexdb::move(task.value());
    }

    inline void drive(coroutine::Task<void> task, EventConsumer& consumer, os::Poll& poll) {
        pump_task(task, consumer, poll);
    }
}
