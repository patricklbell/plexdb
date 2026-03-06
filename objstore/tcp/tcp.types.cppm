module;
#include "macros.h"

export module objstore.tcp.types;

import plexdb.os;
import plexdb.base;
import plexdb.arena;

using namespace plexdb;

export namespace objstore::tcp {
    // ========================================================================
    // Chunk chain
    //   When an inflight request spans multiple read buffer reads, we chain 
    //   buffers together. If the chain grows too large, to avoid starving new
    //   conncetions, we fall back to dynamic allocation. @todo tracking
    // ========================================================================
    constexpr int MAX_SMALL_CHAIN_BUFFERS_COUNT = 4;
    constexpr int LARGE_BUFFER_SIZE = 64 * 1_kb;

    struct Chunk {
        int buffer_idx = -1;
        TArrayView<U8,int> data{};
    };

    using ChunkNode = Deque<Chunk>::Node;

    struct ChunkChain {
        arena::ArenaPage* arena = nullptr;
        CappedArray<ChunkNode, MAX_SMALL_CHAIN_BUFFERS_COUNT> small_chunks;
        Deque<Chunk> chunks;
        U64 chunk_size;

        ChunkChain() = default;
        ~ChunkChain();
    };


    // ========================================================================
    // buffer
    //   a connection can hold multiple buffers, there is a many-to-one mapping
    //   between buffers -> client
    // ========================================================================
    // @note fat struct, not always meaningfull depending on the handler
    struct BufferInfo {
        PLEXDB_DEBUG_X(U32 buffer_idx = -1;)
        os::Handle client = os::zero_handle();
    };

    // ========================================================================
    // connection
    // ========================================================================
    constexpr int MAX_CONCURRENT_CONNECTIONS = 1000;

    struct Connection {
        os::Handle client;
        ChunkChain chunk_chain;
    };

    // ========================================================================
    // user handlers
    // ========================================================================
    enum class RequestStatus {
        // The request has been processed completely
        Handled,
        // The request is pending further data from the client, some data may have been consumed
        Pending,
        // The connection has been processed and should be closed
        Close,
    };

    using AsyncWriteFunctor = AutoFunctor<void(Connection*, U64, U32, U32)>;

    struct Request {
        Connection* connection;
        AsyncWriteFunctor* write;
    };

    // @note this is fired for every new chunk, potentially multiple times per request
    template<typename F>
    concept OnChunk = requires(F f, const Request& req) {
        { f(req) } -> SameAs<RequestStatus>;
    };
    
    template<typename F>
    concept OnOpen = requires(F f, Connection* connection) {
        { f(connection) };
    };

    template<typename F>
    concept OnClose = requires(F f, Connection* connection) {
        { f(connection) };
    };

    template<typename F>
    concept OnReady = requires(F f, U64 buffer_size, U64 buffer_count) {
        { f(buffer_size, buffer_count) };
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