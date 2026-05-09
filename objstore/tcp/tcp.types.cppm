module;
#include "macros.h"
#include <coroutine>

export module objstore.tcp.types;

import plexdb.os;
import plexdb.base;
import plexdb.arena;
import plexdb.coroutine;

using namespace plexdb;

export namespace objstore::tcp {
    constexpr int MAX_CONCURRENT_CONNECTIONS = 1000;
    struct Connection;

    // ========================================================================
    // request handling
    // ========================================================================
    enum class Error {
        None = 0,
        ConnectionClosed,
        // @todo
        Other,
    };

    struct RWBuffer {
        TArrayView<U8,U32> view{};
        U64 length;
        U64 idx;
    };

    using AcquireRWBufferFunctor = AutoFunctor<coroutine::Task<RWBuffer>(Connection*)>;
    using ReleaseRWBufferFunctor = AutoFunctor<void(Connection*,const RWBuffer*)>;
    using AsyncReadFunctor = AutoFunctor<coroutine::Task<Error>(Connection*,RWBuffer*)>;
    using AsyncWriteFunctor = AutoFunctor<coroutine::Task<Error>(Connection*,const RWBuffer*)>;
    using AsyncCloseFunctor = AutoFunctor<coroutine::Task<>(Connection*)>;

    struct Request {
        Connection* connection;
        AcquireRWBufferFunctor* acquire;
        ReleaseRWBufferFunctor* release;
        AsyncReadFunctor* read;
        AsyncWriteFunctor* write;
        AsyncCloseFunctor* close;
    };

    struct Connection {
        os::Handle client;
        Optional<coroutine::Task<void, coroutine::Start::Eager>> task;
        Request req;

        U32 count_rwc;
        Error error_rwc;
        std::coroutine_handle<> waiting_rwc;
    };

    inline coroutine::Task<RWBuffer> acquire(const Request& req) { co_return co_await (*req.acquire)(req.connection); }
    inline void release(const Request& req, const RWBuffer* buffer) { return (*req.release)(req.connection, buffer); }
    coroutine::Task<Error> read(const Request& req, RWBuffer* buffer) { co_return co_await (*req.read)(req.connection, buffer); }
    coroutine::Task<Error> write(const Request& req, const RWBuffer* buffer) { co_return co_await (*req.write)(req.connection, buffer); }
    coroutine::Task<> close(const Request& req) { co_return co_await (*req.close)(req.connection); }
    
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