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
    // ========================================================================
    // connection
    // ========================================================================
    constexpr int MAX_CONCURRENT_CONNECTIONS = 1000;

    struct Connection {
        os::Handle client;
        Optional<coroutine::Task<void, coroutine::Start::Eager>> task;
        
        S64 data_rwc;
        std::coroutine_handle<> waiting_rwc; // bottom-level coroutine waiting for resume after r/w/c completes
    };

    // ========================================================================
    // request handling
    //   @todo error handling for functors
    // ========================================================================
    struct RWBuffer {
        TArrayView<U8,size_t> view{};
        U64 length;
        U64 idx;
    };

    using AcquireRWBufferFunctor = AutoFunctor<Optional<RWBuffer>(Connection*)>;
    using ReleaseRWBufferFunctor = AutoFunctor<void(Connection*,const RWBuffer*)>;
    using AsyncReadFunctor = AutoFunctor<coroutine::Task<bool>(Connection*,RWBuffer*)>;
    using AsyncWriteFunctor = AutoFunctor<coroutine::Task<bool>(Connection*,const RWBuffer*)>;
    using AsyncCloseFunctor = AutoFunctor<coroutine::Task<>(Connection*)>;

    struct Request {
        Connection* connection;
        AcquireRWBufferFunctor* acquire;
        ReleaseRWBufferFunctor* release;
        AsyncReadFunctor* read;
        AsyncWriteFunctor* write;
        AsyncCloseFunctor* close;
    };

    Optional<RWBuffer> acquire(const Request& req) { return (*req.acquire)(req.connection); }
    void release(const Request& req, const RWBuffer* buffer) { return (*req.release)(req.connection, buffer); }
    coroutine::Task<bool> read(const Request& req, RWBuffer* buffer) { co_return co_await (*req.read)(req.connection, buffer); }
    coroutine::Task<bool> write(const Request& req, const RWBuffer* buffer) { co_return co_await (*req.write)(req.connection, buffer); }
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