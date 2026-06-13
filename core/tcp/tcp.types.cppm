module;
#include <plexdb/macros/macros.h>
#include <coroutine>

export module plexdb.tcp.types;

import plexdb.os;
import plexdb.base;
import plexdb.arena;
import plexdb.coroutine;

using namespace plexdb;

export namespace plexdb::tcp {
    constexpr int MAX_CONCURRENT_CONNECTIONS = 1000;
    struct Connection;

    // ========================================================================
    // request handling
    // ========================================================================
    enum class Error {
        None = 0,
        ConnectionClosed,
        Other,
    };

    struct RWBuffer {
        TArrayView<U8, U32> view{};
        U64                 length;
        U64                 idx;
    };

    using AcquireBufferFunctor   = AutoFunctor<coroutine::Task<RWBuffer>(Connection*)>;
    using ReleaseBufferFunctor   = AutoFunctor<void(Connection*, const RWBuffer*)>;
    using ReadToBufferFunctor    = AutoFunctor<coroutine::Task<Error>(Connection*, RWBuffer*)>;
    using WriteFromBufferFunctor = AutoFunctor<coroutine::Task<Error>(Connection*, const RWBuffer*)>;

    struct Request {
        Connection*             connection;
        AcquireBufferFunctor*   acquire;
        ReleaseBufferFunctor*   release;
        ReadToBufferFunctor*    read;
        WriteFromBufferFunctor* write;
    };

    struct Connection {
        os::Handle                                               client;
        Optional<coroutine::Task<void, coroutine::Start::Eager>> task;
        Request                                                  req;

        U32                     count_rwc;
        Error                   error_rwc;
        std::coroutine_handle<> waiting_rwc;
    };

    inline coroutine::Task<RWBuffer> acquire(const Request& req) {
        co_return co_await (*req.acquire)(req.connection);
    }
    inline void release(const Request& req, const RWBuffer* buffer) {
        return (*req.release)(req.connection, buffer);
    }
    inline coroutine::Task<Error> read(const Request& req, RWBuffer* buffer) {
        co_return co_await (*req.read)(req.connection, buffer);
    }
    inline coroutine::Task<Error> write(const Request& req, const RWBuffer* buffer) {
        co_return co_await (*req.write)(req.connection, buffer);
    }

    template<typename F>
    concept ConnectionHandler = requires(F f, Request connection) {
        { f(connection) } -> SameAs<coroutine::Task<void, coroutine::Start::Eager>>;
    };

    // ========================================================================
    // stats
    // ========================================================================
    struct Stats {
        U64 dropped_connections = 0;
        U64 total_connections   = 0;
        U64 active_connections  = 0;
        U64 total_bytes_read    = 0;
        U64 total_bytes_written = 0;
    };
}
