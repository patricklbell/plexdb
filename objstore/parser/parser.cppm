export module objstore.parser;

import plexdb.base;
import plexdb.tagged_union;
import objstore.engine.dtype;
import objstore.engine.statements;

using namespace plexdb;

namespace objstore::parser {
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

        // ====================================================================
        // incremental parser
        // ====================================================================
        struct ParserState;

        ParserState* parser_create();
        void parser_reset(ParserState* state);
        void parser_destroy(ParserState* state);
        void parser_execute(ParserState* state, const char* data, U64 length);
        const Request& parser_get_request(const ParserState* state);
        bool parser_is_complete(const ParserState* state);
        bool parser_has_error(const ParserState* state);

        export struct Parser {
            ParserState* state;

            Parser() : state(parser_create()) {}
            ~Parser() { if (state) parser_destroy(state); }

            Parser(const Parser&) = delete;
            Parser& operator=(const Parser&) = delete;

            Parser(Parser&& other) noexcept : state(other.state) { other.state = nullptr; }
            Parser& operator=(Parser&& other) noexcept {
                if (this != &other) {
                    if (state) parser_destroy(state);
                    state = other.state;
                    other.state = nullptr;
                }
                return *this;
            }
        };

        export inline void reset(Parser& parser) { parser_reset(parser.state); }
        export inline void execute(Parser& parser, const char* data, U64 length) { parser_execute(parser.state, data, length); }
        export inline const Request& get_request(const Parser& parser) { return parser_get_request(parser.state); }
        export inline bool is_complete(const Parser& parser) { return parser_is_complete(parser.state); }
        export inline bool has_error(const Parser& parser) { return parser_has_error(parser.state); }
    }

    // ========================================================================
    // cassandra query language (CQL)
    // ========================================================================
    namespace cql {
        export Optional<Statement> parse(String8 bytes, bool stderr=false);
        export bool is_complete(String8 bytes);
    }
}
