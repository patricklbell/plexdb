module;
#include <coroutine>

export module keyvalue.resp;

import plexdb.base;
import plexdb.os;
import plexdb.aio;
import plexdb.tcp;
import plexdb.coroutine;
import plexdb.dynamic.containers;
import keyvalue.parsers;
import keyvalue.engine;
import keyvalue.log;
import keyvalue.resp.protocol;

using namespace plexdb;
using namespace plexdb::tcp;

export namespace keyvalue::resp {
    template<typename F>
    concept OnReady = requires(F f) { f(); };

    Optional<String8> run(
        U16 port,
        Engine auto& engine,
        const OnReady auto& on_ready_callback,
        bool use_uring,
        aio::EventConsumer& signal_consumer,
        os::Poll& io_poll
    ) {
        const auto try_append = [](const tcp::Request& req, DynamicArray<U8>& buf) -> coroutine::Task<bool> {
            RWBuffer rbuf = co_await tcp::acquire(req);
            Error err = co_await tcp::read(req, &rbuf);
            if (err != Error::None) {
                tcp::release(req, &rbuf);
                co_return false;
            }
            U64 old = buf.length;
            resize(buf, old + rbuf.view.length);
            os::memory_copy(buf.ptr + old, rbuf.view.ptr, rbuf.view.length);
            tcp::release(req, &rbuf);
            co_return true;
        };

        const auto send_block = [](const tcp::Request& req, const U8* data, U64 len) -> coroutine::Task<> {
            U64 sent = 0;
            while (sent < len) {
                RWBuffer wbuf = co_await tcp::acquire(req);
                U64 chunk = min(len - sent, wbuf.length);
                os::memory_copy(wbuf.view.ptr, data + sent, chunk);
                wbuf.view.length = U32(chunk);
                co_await tcp::write(req, &wbuf);
                tcp::release(req, &wbuf);
                sent += chunk;
            }
        };

        S64 active_connections = 0;

        const auto connection_handler = [&engine, &try_append, &send_block, &active_connections](const tcp::Request& req)
            -> coroutine::Task<void, coroutine::Start::Eager>
        {
            log::db_connection_count(++active_connections);
            DynamicArray<U8> read_buf{};
            DynamicArray<U8> write_buf{};

            while (true) {
                U64 consumed = 0;
                auto [parse_res, stmt] = parsers::parse(read_buf.ptr, read_buf.length, &consumed);

                if (parse_res == parsers::ParseResult::Incomplete) {
                    if (!co_await try_append(req, read_buf)) {
                        log::db_connection_count(--active_connections);
                        co_return;
                    }
                    continue;
                }

                if (parse_res == parsers::ParseResult::Error) {
                    clear(write_buf);
                    protocol::append_error(write_buf, "ERR", "Protocol error");
                    co_await send_block(req, write_buf.ptr, write_buf.length);
                    log::db_connection_count(--active_connections);
                    co_return;
                }

                clear(write_buf);

                S64 t0 = os::monotonic_us();
                engine::ExecutionResult result = co_await engine::execute(engine, stmt);
                log::db_operation_duration(os::monotonic_us() - t0);

                bool keep_alive = protocol::encode_result(result, write_buf);

                if (write_buf.length > 0)
                    co_await send_block(req, write_buf.ptr, write_buf.length);

                U64 remaining = read_buf.length - consumed;
                if (remaining > 0)
                    os::memory_move(read_buf.ptr, read_buf.ptr + consumed, remaining);
                resize(read_buf, remaining);

                if (!keep_alive) {
                    log::db_connection_count(--active_connections);
                    co_return;
                }
            }
        };

        {
            os::Socket socket{os::socket_open()};
            if (!socket)
                return {"failed to open server socket"};
            if (!os::socket_set_option(socket, os::SocketOption::Reuse, true))
                return {"failed to set reuse on server socket"};
            if (!os::socket_bind(socket, port))
                return {"failed to bind server socket"};
            if (!os::socket_listen(socket, 128))
                return {"failed to listen on server socket"};
            log::db_connection_max(128);

            auto tcp_server = tcp::create_tcp_server(socket, &connection_handler, io_poll, use_uring);
            on_ready_callback();
            aio::run_blocking_event_loop(io_poll, signal_consumer, tcp_server.consumer);
        }

        return {};
    }
}
