export module plexdb.test.server_helpers;

import plexdb.os;
import plexdb.aio;
import plexdb.threads;

using namespace plexdb;

export {
    // Launch client_fn on a background thread and server_fn on the calling
    // (main) thread. client_fn receives the interrupt notifier and must call
    // os::signal_notify_safe(interrupt) when done. server_fn receives
    // (on_ready, signal_consumer, poll) — it should call on_ready() once the
    // server is accepting, and stop when signal_consumer fires.
    template<typename ClientFn, typename ServerFn>
    void run_server_test(const char* thread_name, ClientFn&& client_fn, ServerFn&& server_fn) {
        os::Notifier       interrupt{};
        threads::Semaphore ready{0};

        threads::Thread client_thread = threads::launch(thread_name, [&]() {
            ready.wait();
            client_fn(interrupt);
        });

        os::Poll poll{};
        auto     signal_consumer = aio::create_notifier_consumer(interrupt, poll);
        server_fn([&ready]() { ready.signal(); }, signal_consumer, poll);
    }
}
