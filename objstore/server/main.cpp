#include <stdlib.h>

import objstore.tcp;
import objstore.parser;
import plexdb.base;
import plexdb.os;

using namespace objstore;
using namespace plexdb;

void assert_handler(const char* msg, const char* file_name, const char* function_name, unsigned line_number) {
    println(msg);
    exit(1);
}

static void text(tcp::Request& req, String8 msg) {
    for (int offset = 0; offset < msg.length; offset += req.inout_data.length) {
        int length = min((int)msg.length - offset, req.inout_data.length);
        os::memory_copy(req.inout_data.ptr, &msg.data[offset], length);
        req.inout_data.prefix = length;

        tcp::submit_write(req);
    }
}

static void success(tcp::Request& req, const String8& body) {
    text(req, fmt(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "%s",
        static_cast<int>(body.length),
        body.c_str()
    ));
}

static void fail(tcp::Request& req, int status, const String8& body, bool close=false) {
    auto get_reason_for_status = [](int code) -> const char* {
        switch (code) {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    };

    static const char* connection_open = "";
    static const char* connection_close = "Connection: close\r\n";

    text(req, fmt(
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/plain\r\n"
        "%s"
        "\r\n"
        "%s",
        status,
        get_reason_for_status(status),
        static_cast<int>(body.length),
        (close ? connection_close : connection_open),
        body.c_str()
    ));
}

int main(int argc, char* argv[]) {
    set_assert_handler(assert_handler);

    int port = 8080;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // @todo detect sys info including permissions

    tcp::Pool pool{port};
    tcp::Stats stats{};

    println("listening on port ", to_str(port));

    const auto echo = [&stats](tcp::Request& req) -> void {
        auto http_request_opt = parser::parse_request(String8(req.inout_data));
        if (!http_request_opt) {
            return fail(req, 400, "Bad request", /*close*/ true);
        }

        auto cql_opt = parser::parse_cql(http_request_opt->body);
        if (!cql_opt) {
            // @todo return parse error message
            return fail(req, 400, "Bad request");
        }
        return success(req, "success");        
    };
    tcp::listen(echo, pool, stats);
    
    return 0;
}