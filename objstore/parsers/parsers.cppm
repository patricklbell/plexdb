export module objstore.parsers;

import plexdb.base;
import plexdb.log;
import plexdb.tagged_union;
import objstore.engine.dtype;
import objstore.engine.statements;

using namespace plexdb;

namespace objstore::parsers {
    // ========================================================================
    // hypertext transfer protocol (HTTP)
    // ========================================================================
    namespace http {
        constexpr U64 MAX_HEADERS = 32;
        constexpr U64 MAX_URL_SIZE = 2048;
        constexpr U64 MAX_HEADER_NAME_SIZE = 256;
        constexpr U64 MAX_HEADER_VALUE_SIZE = 4096;
        constexpr U64 MAX_METHOD_SIZE = 16;

        export struct Header {
            String8 name;
            String8 value;
        };

        export struct Request {
            String8 method;
            String8 url;
            Array<Header, MAX_HEADERS> headers;
            U64 header_count = 0;
            String8 body;
        };

        export struct Response {
            U16 status_code;
            String8 status_message;
            Array<Header, MAX_HEADERS> headers;
            U64 header_count = 0;
            String8 body;
        };

        // ====================================================================
        // request parser
        // ====================================================================
        struct RequestParserState;

        RequestParserState* request_parser_create();
        void request_parser_reset(RequestParserState* state);
        void request_parser_destroy(RequestParserState* state);
        void request_parser_execute(RequestParserState* state, const char* data, U64 length);
        const Request& request_parser_get_request(const RequestParserState* state);
        bool request_parser_is_complete(const RequestParserState* state);
        bool request_parser_has_error(const RequestParserState* state);

        export struct RequestParser {
            RequestParserState* state;

            RequestParser() : state(request_parser_create()) {}
            ~RequestParser() { if (state) request_parser_destroy(state); }

            RequestParser(const RequestParser&) = delete;
            RequestParser& operator=(const RequestParser&) = delete;

            RequestParser(RequestParser&& other) noexcept : state(other.state) { other.state = nullptr; }
            RequestParser& operator=(RequestParser&& other) noexcept {
                if (this != &other) {
                    if (state) request_parser_destroy(state);
                    state = other.state;
                    other.state = nullptr;
                }
                return *this;
            }
        };

        export inline void reset(RequestParser& parser) { request_parser_reset(parser.state); }
        export inline void execute(RequestParser& parser, const char* data, U64 length) { request_parser_execute(parser.state, data, length); }
        export inline const Request& get_request(const RequestParser& parser) { return request_parser_get_request(parser.state); }
        export inline bool is_complete(const RequestParser& parser) { return request_parser_is_complete(parser.state); }
        export inline bool has_error(const RequestParser& parser) { return request_parser_has_error(parser.state); }

        // ====================================================================
        // response parser
        // ====================================================================
        struct ResponseParserState;

        ResponseParserState* response_parser_create();
        void response_parser_reset(ResponseParserState* state);
        void response_parser_destroy(ResponseParserState* state);
        void response_parser_execute(ResponseParserState* state, const char* data, U64 length);
        const Response& response_parser_get_response(const ResponseParserState* state);
        bool response_parser_is_complete(const ResponseParserState* state);
        bool response_parser_has_error(const ResponseParserState* state);

        export struct ResponseParser {
            ResponseParserState* state;

            ResponseParser() : state(response_parser_create()) {}
            ~ResponseParser() { if (state) response_parser_destroy(state); }

            ResponseParser(const ResponseParser&) = delete;
            ResponseParser& operator=(const ResponseParser&) = delete;

            ResponseParser(ResponseParser&& other) noexcept : state(other.state) { other.state = nullptr; }
            ResponseParser& operator=(ResponseParser&& other) noexcept {
                if (this != &other) {
                    if (state) response_parser_destroy(state);
                    state = other.state;
                    other.state = nullptr;
                }
                return *this;
            }
        };

        export inline void reset(ResponseParser& parser) { response_parser_reset(parser.state); }
        export inline void execute(ResponseParser& parser, const char* data, U64 length) { response_parser_execute(parser.state, data, length); }
        export inline const Response& get_response(const ResponseParser& parser) { return response_parser_get_response(parser.state); }
        export inline bool is_complete(const ResponseParser& parser) { return response_parser_is_complete(parser.state); }
        export inline bool has_error(const ResponseParser& parser) { return response_parser_has_error(parser.state); }
    }

    // ========================================================================
    // cassandra query language (CQL)
    // ========================================================================
    namespace cql {
        export Optional<Statement> parse(String8 bytes, bool report_errors=log::enabled);
        export Optional<Statement> parse(String8 bytes, void(*error_fn)(const char*, size_t));
        export bool is_complete(String8 bytes);
    }
}
