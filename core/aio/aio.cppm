module;
#include <plexdb/macros/macros.h>
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
    using OnBlockFunctor   = AutoFunctor<void()>;
    struct EventConsumer {
        U64              max_events;
        OnUnblockFunctor on_unblock;

        // @optional
        // @note runs for every consumer right before the loop blocks, so a coroutine
        // resumed by one consumer that queues work on another consumer is guaranteed
        // to be submitted before we wait on the poll
        OnBlockFunctor on_block{};
    };

    template<typename... Cs>
    void run_blocking_event_loop(const os::Poll& unblock, Cs&... consumers) {
        U64 total_max_events = (consumers.max_events + ... + U64(0));
        if (total_max_events == 0) {
            total_max_events = 1;
        }

        threads::Scope                  scratch        = threads::scratch();
        os::PollEvent*                  events_storage = arena::push_array<os::PollEvent>(*scratch.arena, total_max_events);
        CappedTArrayView<os::PollEvent> events_view{events_storage, total_max_events, 0};

        while (true) {
            auto on_block = [&](auto& c) {
                if (c.on_block) {
                    c.on_block();
                }
            };
            (on_block(consumers), ...);

            events_view.cap = 0;
            os::block_until_poll_unblocks_wth_events(unblock, &events_view);
            TArrayView<os::PollEvent> events{events_view.ptr, events_view.cap};

            bool stop       = false;
            auto on_unblock = [&](auto& c) {
                stop |= !c.on_unblock(events);
            };
            (on_unblock(consumers), ...);

            if (stop) {
                break;
            }
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

    coroutine::Task<> file_read(FileIOContext& ctx, os::Handle file, Rng1U64 rng, void* out);
    coroutine::Task<> file_write(FileIOContext& ctx, os::Handle file, Rng1U64 rng, const void* in);
    coroutine::Task<> file_sync(FileIOContext& ctx, os::Handle file);

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
        U64                             max_event_count = consumer.max_events > 0 ? consumer.max_events : 1;
        threads::Scope                  scratch         = threads::scratch();
        os::PollEvent*                  events_storage  = arena::push_array<os::PollEvent>(*scratch.arena, max_event_count);
        CappedTArrayView<os::PollEvent> events_view{events_storage, max_event_count, 0};

        task.resume();
        while (!task.done()) {
            if (consumer.on_block) {
                consumer.on_block();
            }

            events_view.cap = 0;
            os::block_until_poll_unblocks_wth_events(poll, &events_view);
            TArrayView<os::PollEvent> events{events_view.ptr, events_view.cap};

            assert_true_always(consumer.on_unblock(events), "event consumer stopping in pumped task is not implemented");
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
