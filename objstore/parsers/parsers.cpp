module;
#include <llhttp.h>

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/error.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/input_location.hpp>

#define LEXY_LIT_CI(x) lexy::dsl::ascii::case_folding(LEXY_LIT(x))

module objstore.parsers;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;
import objstore.log;

namespace objstore::parsers::http {
    // ========================================================================
    // request parser
    // ========================================================================
    // @note accumulates data since llhttp callbacks point into current input buffer only
    struct RequestParserState {
        llhttp_t parser;
        Request request;
        
        // Accumulation buffers for spans that may cross chunk boundaries
        char method_buf[MAX_METHOD_SIZE];
        U64 method_len = 0;
        
        char url_buf[MAX_URL_SIZE];
        U64 url_len = 0;
        
        char header_name_buf[MAX_HEADER_NAME_SIZE];
        U64 header_name_len = 0;
        
        char header_value_buf[MAX_HEADER_VALUE_SIZE];
        U64 header_value_len = 0;
        
        // Body is typically large, so we allocate dynamically
        char* body_buf = nullptr;
        U64 body_capacity = 0;
        U64 body_len = 0;
        
        bool message_complete = false;
        bool has_error = false;
        bool in_header_value = false;
    };

    static int inc_on_method(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        U64 copy_len = min(length, MAX_METHOD_SIZE - state->method_len);
        os::memory_copy(state->method_buf + state->method_len, at, copy_len);
        state->method_len += copy_len;
        return 0;
    }

    static int inc_on_method_complete(llhttp_t* parser) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        state->request.method = {reinterpret_cast<const U8*>(state->method_buf), state->method_len};
        return 0;
    }

    static int inc_on_url(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        U64 copy_len = min(length, MAX_URL_SIZE - state->url_len);
        os::memory_copy(state->url_buf + state->url_len, at, copy_len);
        state->url_len += copy_len;
        return 0;
    }

    static int inc_on_url_complete(llhttp_t* parser) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        state->request.url = {reinterpret_cast<const U8*>(state->url_buf), state->url_len};
        return 0;
    }

    static int inc_on_header_field(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        if (state->in_header_value && state->request.header_count < MAX_HEADERS) {
            state->request.headers.values[state->request.header_count].name = 
                {reinterpret_cast<const U8*>(state->header_name_buf), state->header_name_len};
            state->request.headers.values[state->request.header_count].value = 
                {reinterpret_cast<const U8*>(state->header_value_buf), state->header_value_len};
            state->request.header_count++;
            state->header_name_len = 0;
            state->header_value_len = 0;
            state->in_header_value = false;
        }
        
        U64 copy_len = min(length, MAX_HEADER_NAME_SIZE - state->header_name_len);
        os::memory_copy(state->header_name_buf + state->header_name_len, at, copy_len);
        state->header_name_len += copy_len;
        return 0;
    }

    static int inc_on_header_value(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        state->in_header_value = true;
        
        U64 copy_len = min(length, MAX_HEADER_VALUE_SIZE - state->header_value_len);
        os::memory_copy(state->header_value_buf + state->header_value_len, at, copy_len);
        state->header_value_len += copy_len;
        return 0;
    }

    static int inc_on_headers_complete(llhttp_t* parser) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        if (state->in_header_value && state->request.header_count < MAX_HEADERS) {
            state->request.headers.values[state->request.header_count].name = 
                {reinterpret_cast<const U8*>(state->header_name_buf), state->header_name_len};
            state->request.headers.values[state->request.header_count].value = 
                {reinterpret_cast<const U8*>(state->header_value_buf), state->header_value_len};
            state->request.header_count++;
            state->in_header_value = false;
        }
        return 0;
    }

    static int inc_on_body(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        if (state->body_len + length > state->body_capacity) {
            U64 new_capacity = max(state->body_capacity * 2, state->body_len + length);
            new_capacity = max(new_capacity, 4096_u64); // Minimum 4KB
            
            char* new_buf = reinterpret_cast<char*>(os::allocate(new_capacity));
            if (state->body_buf) {
                os::memory_copy(new_buf, state->body_buf, state->body_len);
                os::deallocate(state->body_buf);
            }
            state->body_buf = new_buf;
            state->body_capacity = new_capacity;
        }
        
        os::memory_copy(state->body_buf + state->body_len, at, length);
        state->body_len += length;
        return 0;
    }

    static int inc_on_message_complete(llhttp_t* parser) {
        auto* state = static_cast<RequestParserState*>(parser->data);
        state->request.body = {reinterpret_cast<const U8*>(state->body_buf), state->body_len};
        state->message_complete = true;
        return 0;
    }

    static llhttp_settings_t& get_incremental_settings() {
        static llhttp_settings_t settings;
        static bool initialized = false;
        if (!initialized) {
            llhttp_settings_init(&settings);
            settings.on_method = inc_on_method;
            settings.on_method_complete = inc_on_method_complete;
            settings.on_url = inc_on_url;
            settings.on_url_complete = inc_on_url_complete;
            settings.on_header_field = inc_on_header_field;
            settings.on_header_value = inc_on_header_value;
            settings.on_headers_complete = inc_on_headers_complete;
            settings.on_body = inc_on_body;
            settings.on_message_complete = inc_on_message_complete;
            initialized = true;
        }
        return settings;
    }

    RequestParserState* request_parser_create() {
        auto* state = reinterpret_cast<RequestParserState*>(os::allocate_zero(sizeof(RequestParserState)));
        llhttp_init(&state->parser, HTTP_REQUEST, &get_incremental_settings());
        state->parser.data = state;
        return state;
    }

    void request_parser_reset(RequestParserState* state) {
        if (state->body_buf) {
            os::deallocate(state->body_buf);
        }
        llhttp_reset(&state->parser);
        state->request = Request{};
        state->method_len = 0;
        state->url_len = 0;
        state->header_name_len = 0;
        state->header_value_len = 0;
        state->body_buf = nullptr;
        state->body_capacity = 0;
        state->body_len = 0;
        state->message_complete = false;
        state->has_error = false;
        state->in_header_value = false;
    }

    void request_parser_destroy(RequestParserState* state) {
        if (state) {
            if (state->body_buf) {
                os::deallocate(state->body_buf);
            }
            os::deallocate(state);
        }
    }

    void request_parser_execute(RequestParserState* state, const char* data, U64 length) {
        if (state->has_error || state->message_complete) {
            return;
        }
        
        llhttp_errno err = llhttp_execute(&state->parser, data, length);
        
        if (err != HPE_OK) {
            state->has_error = true;
        }
    }

    const Request& request_parser_get_request(const RequestParserState* state) {
        return state->request;
    }

    bool request_parser_is_complete(const RequestParserState* state) {
        return state->message_complete;
    }

    bool request_parser_has_error(const RequestParserState* state) {
        return state->has_error;
    }

    // ========================================================================
    // response parser
    // ========================================================================
    struct ResponseParserState {
        llhttp_t parser;
        Response response;
        
        char status_buf[256];
        U64 status_len = 0;
        
        char header_name_buf[MAX_HEADER_NAME_SIZE];
        U64 header_name_len = 0;
        
        char header_value_buf[MAX_HEADER_VALUE_SIZE];
        U64 header_value_len = 0;
        
        char* body_buf = nullptr;
        U64 body_capacity = 0;
        U64 body_len = 0;
        
        bool message_complete = false;
        bool has_error = false;
        bool in_header_value = false;
    };

    static int resp_on_status(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        U64 copy_len = min(length, 256 - state->status_len);
        os::memory_copy(state->status_buf + state->status_len, at, copy_len);
        state->status_len += copy_len;
        return 0;
    }

    static int resp_on_status_complete(llhttp_t* parser) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        state->response.status_code = static_cast<U16>(parser->status_code);
        state->response.status_message = {reinterpret_cast<const U8*>(state->status_buf), state->status_len};
        return 0;
    }

    static int resp_on_header_field(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        if (state->in_header_value && state->response.header_count < MAX_HEADERS) {
            state->response.headers.values[state->response.header_count].name = 
                {reinterpret_cast<const U8*>(state->header_name_buf), state->header_name_len};
            state->response.headers.values[state->response.header_count].value = 
                {reinterpret_cast<const U8*>(state->header_value_buf), state->header_value_len};
            state->response.header_count++;
            state->header_name_len = 0;
            state->header_value_len = 0;
            state->in_header_value = false;
        }
        
        U64 copy_len = min(length, MAX_HEADER_NAME_SIZE - state->header_name_len);
        os::memory_copy(state->header_name_buf + state->header_name_len, at, copy_len);
        state->header_name_len += copy_len;
        return 0;
    }

    static int resp_on_header_value(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        state->in_header_value = true;
        U64 copy_len = min(length, MAX_HEADER_VALUE_SIZE - state->header_value_len);
        os::memory_copy(state->header_value_buf + state->header_value_len, at, copy_len);
        state->header_value_len += copy_len;
        return 0;
    }

    static int resp_on_headers_complete(llhttp_t* parser) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        if (state->in_header_value && state->response.header_count < MAX_HEADERS) {
            state->response.headers.values[state->response.header_count].name = 
                {reinterpret_cast<const U8*>(state->header_name_buf), state->header_name_len};
            state->response.headers.values[state->response.header_count].value = 
                {reinterpret_cast<const U8*>(state->header_value_buf), state->header_value_len};
            state->response.header_count++;
        }
        state->header_name_len = 0;
        state->header_value_len = 0;
        state->in_header_value = false;
        return 0;
    }

    static int resp_on_body(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        
        if (state->body_len + length > state->body_capacity) {
            U64 new_capacity = max(state->body_capacity * 2, state->body_len + length + 1024);
            char* new_buf = reinterpret_cast<char*>(os::allocate(new_capacity));
            if (state->body_buf) {
                os::memory_copy(new_buf, state->body_buf, state->body_len);
                os::deallocate(state->body_buf);
            }
            state->body_buf = new_buf;
            state->body_capacity = new_capacity;
        }
        
        os::memory_copy(state->body_buf + state->body_len, at, length);
        state->body_len += length;
        return 0;
    }

    static int resp_on_message_complete(llhttp_t* parser) {
        auto* state = static_cast<ResponseParserState*>(parser->data);
        state->response.body = {reinterpret_cast<const U8*>(state->body_buf), state->body_len};
        state->message_complete = true;
        return 0;
    }

    static llhttp_settings_t& get_response_settings() {
        static llhttp_settings_t settings;
        static bool initialized = false;
        if (!initialized) {
            llhttp_settings_init(&settings);
            settings.on_status = resp_on_status;
            settings.on_status_complete = resp_on_status_complete;
            settings.on_header_field = resp_on_header_field;
            settings.on_header_value = resp_on_header_value;
            settings.on_headers_complete = resp_on_headers_complete;
            settings.on_body = resp_on_body;
            settings.on_message_complete = resp_on_message_complete;
            initialized = true;
        }
        return settings;
    }

    ResponseParserState* response_parser_create() {
        auto* state = reinterpret_cast<ResponseParserState*>(os::allocate_zero(sizeof(ResponseParserState)));
        llhttp_init(&state->parser, HTTP_RESPONSE, &get_response_settings());
        state->parser.data = state;
        return state;
    }

    void response_parser_reset(ResponseParserState* state) {
        if (state->body_buf) {
            os::deallocate(state->body_buf);
        }
        llhttp_reset(&state->parser);
        state->response = Response{};
        state->status_len = 0;
        state->header_name_len = 0;
        state->header_value_len = 0;
        state->body_buf = nullptr;
        state->body_capacity = 0;
        state->body_len = 0;
        state->message_complete = false;
        state->has_error = false;
        state->in_header_value = false;
    }

    void response_parser_destroy(ResponseParserState* state) {
        if (state) {
            if (state->body_buf) {
                os::deallocate(state->body_buf);
            }
            os::deallocate(state);
        }
    }

    void response_parser_execute(ResponseParserState* state, const char* data, U64 length) {
        if (state->has_error || state->message_complete) {
            return;
        }
        
        llhttp_errno err = llhttp_execute(&state->parser, data, length);
        
        if (err != HPE_OK) {
            state->has_error = true;
        }
    }

    const Response& response_parser_get_response(const ResponseParserState* state) {
        return state->response;
    }

    bool response_parser_is_complete(const ResponseParserState* state) {
        return state->message_complete;
    }

    bool response_parser_has_error(const ResponseParserState* state) {
        return state->has_error;
    }
}

namespace objstore::parsers::cql {
    // ========================================================================
    // cassandra query language (CQL)
    // ========================================================================
    namespace grammar {
        namespace dsl = lexy::dsl;
    
        // whitespace
        struct ws {
            static constexpr auto rule = dsl::whitespace(dsl::ascii::space / dsl::ascii::newline);
            static constexpr auto value = lexy::noop;
        };
    
        // keywords (case-insensitive)
        constexpr auto kw_create = LEXY_LIT_CI("create");
        constexpr auto kw_alter = LEXY_LIT_CI("alter");
        constexpr auto kw_drop = LEXY_LIT_CI("drop");
        constexpr auto kw_truncate = LEXY_LIT_CI("truncate");
        constexpr auto kw_keyspace = LEXY_LIT_CI("keyspace");
        constexpr auto kw_table = LEXY_LIT_CI("table");
        constexpr auto kw_columnfamily = LEXY_LIT_CI("columnfamily");
        constexpr auto kw_if = LEXY_LIT_CI("if");
        constexpr auto kw_not = LEXY_LIT_CI("not");
        constexpr auto kw_exists = LEXY_LIT_CI("exists");
        constexpr auto kw_insert = LEXY_LIT_CI("insert");
        constexpr auto kw_into = LEXY_LIT_CI("into");
        constexpr auto kw_values = LEXY_LIT_CI("values");
        constexpr auto kw_select = LEXY_LIT_CI("select");
        constexpr auto kw_from = LEXY_LIT_CI("from");
        constexpr auto kw_with = LEXY_LIT_CI("with");
        constexpr auto kw_primary = LEXY_LIT_CI("primary");
        constexpr auto kw_key = LEXY_LIT_CI("key");
        constexpr auto kw_and = LEXY_LIT_CI("and");
        constexpr auto kw_use = LEXY_LIT_CI("use");
        constexpr auto kw_update = LEXY_LIT_CI("update");
        constexpr auto kw_set = LEXY_LIT_CI("set");
        constexpr auto kw_where = LEXY_LIT_CI("where");
        constexpr auto kw_delete = LEXY_LIT_CI("delete");
        constexpr auto kw_using = LEXY_LIT_CI("using");
        constexpr auto kw_timestamp = LEXY_LIT_CI("timestamp");
        constexpr auto kw_ttl = LEXY_LIT_CI("ttl");
        constexpr auto kw_begin = LEXY_LIT_CI("begin");
        constexpr auto kw_apply = LEXY_LIT_CI("apply");
        constexpr auto kw_batch = LEXY_LIT_CI("batch");
        constexpr auto kw_unlogged = LEXY_LIT_CI("unlogged");
        constexpr auto kw_counter = LEXY_LIT_CI("counter");
        constexpr auto kw_add = LEXY_LIT_CI("add");
        constexpr auto kw_rename = LEXY_LIT_CI("rename");
        constexpr auto kw_to = LEXY_LIT_CI("to");
        constexpr auto kw_limit = LEXY_LIT_CI("limit");
        constexpr auto kw_in = LEXY_LIT_CI("in");
        constexpr auto kw_contains = LEXY_LIT_CI("contains");
        constexpr auto kw_true = LEXY_LIT_CI("true");
        constexpr auto kw_false = LEXY_LIT_CI("false");

        // identifiers
        struct identifier {
            static constexpr auto rule = dsl::identifier(dsl::ascii::alpha_underscore, 
                                                        dsl::ascii::alpha_digit_underscore);
            static constexpr auto value = lexy::as_string<String8>;
        };

        struct data_type {
            struct text : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("text");
                static constexpr auto value = lexy::constant(DType::text);
            };
            struct int_ : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("int");
                static constexpr auto value = lexy::constant(DType::int_);
            };
            struct bigint : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("bigint");
                static constexpr auto value = lexy::constant(DType::bigint);
            };
            struct smallint : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("smallint");
                static constexpr auto value = lexy::constant(DType::smallint);
            };
            struct counter : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("counter");
                static constexpr auto value = lexy::constant(DType::counter);
            };
            struct timestamp : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("timestamp");
                static constexpr auto value = lexy::constant(DType::timestamp);
            };
            struct boolean : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("boolean");
                static constexpr auto value = lexy::constant(DType::boolean);
            };
            struct float_ : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("float");
                static constexpr auto value = lexy::constant(DType::float_);
            };
            struct double_ : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("double");
                static constexpr auto value = lexy::constant(DType::double_);
            };
            struct uuid : lexy::transparent_production {
                static constexpr auto rule  = LEXY_LIT_CI("uuid");
                static constexpr auto value = lexy::constant(DType::uuid);
            };

            static constexpr auto rule  = dsl::p<text> | dsl::p<int_> | dsl::p<bigint> | dsl::p<smallint> | dsl::p<counter> | dsl::p<timestamp> | dsl::p<boolean> | dsl::p<float_> | dsl::p<double_> | dsl::p<uuid>;
            static constexpr auto value = lexy::forward<DType>;
        };
    
        // string literal
        // @note supports both backslash escapes (common in drivers) and $$ delimiters
        // @todo add CQL '' escape support
        struct string_literal {
            static constexpr auto escaped_symbols = lexy::symbol_table<char>
                                                        .map<'\''>('\'')
                                                        .map<'\\'>('\\')
                                                        .map<'/'>('/')
                                                        .map<'b'>('\b')
                                                        .map<'f'>('\f')
                                                        .map<'n'>('\n')
                                                        .map<'r'>('\r')
                                                        .map<'t'>('\t');
                                                        
            static constexpr auto rule = [] {
                auto escape = dsl::backslash_escape.symbol<escaped_symbols>();    
                return dsl::delimited(LEXY_LIT("'") | LEXY_LIT("$$"))(-dsl::ascii::control, escape);
            }();
            
            // @todo arena allocator, this needs allocation because to resolve escape characters
            static constexpr auto value = lexy::as_string<AutoString8>;
        };
    
        // integer literal
        struct integer_literal {
            static constexpr auto rule = []() {
                auto plus_minus = dsl::lit_c<'-'> / dsl::lit_c<'-'>;
                return dsl::peek(plus_minus / dsl::digit<>) >> dsl::sign + dsl::integer<S64>;
            }();
            static constexpr auto value = lexy::as_integer<S64>;
        };

        struct boolean_literal {
            struct true_ : lexy::transparent_production {
                static constexpr auto rule  = kw_true;
                static constexpr auto value = lexy::constant(true);
            };
            struct false_ : lexy::transparent_production {
                static constexpr auto rule  = kw_false;
                static constexpr auto value = lexy::constant(false);
            };
            static constexpr auto rule = dsl::p<true_> | dsl::p<false_>;
            static constexpr auto value = lexy::forward<bool>;
        };

        // ====================================================================
        // common clauses
        // ====================================================================

        // IF NOT EXISTS clause
        struct if_not_exists {
            static constexpr auto rule = []() {
                auto key = kw_if + dsl::p<ws> + kw_not + dsl::p<ws> + kw_exists;
                return dsl::opt(dsl::peek(key) >> key);
            }();

            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };

        // IF EXISTS clause
        struct if_exists {
            static constexpr auto rule = []() {
                auto key = kw_if + dsl::p<ws> + kw_exists;
                return dsl::opt(dsl::peek(key) >> key);
            }();

            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };

        // table name with keyspace (ks.table)
        struct table_ref {
            static constexpr auto rule = [] {
                return (dsl::member<&TableRef::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                       (dsl::member<&TableRef::table_name> = dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::as_aggregate<TableRef>;
        };

        // comparison operators
        struct comparison_op {
            struct eq : lexy::transparent_production {
                static constexpr auto rule = dsl::lit_c<'='>;
                static constexpr auto value = lexy::constant(ComparisonOp::eq);
            };
            struct ne : lexy::transparent_production {
                static constexpr auto rule = LEXY_LIT("!=");
                static constexpr auto value = lexy::constant(ComparisonOp::ne);
            };
            struct le : lexy::transparent_production {
                static constexpr auto rule = LEXY_LIT("<=");
                static constexpr auto value = lexy::constant(ComparisonOp::le);
            };
            struct lt : lexy::transparent_production {
                static constexpr auto rule = dsl::lit_c<'<'>;
                static constexpr auto value = lexy::constant(ComparisonOp::lt);
            };
            struct ge : lexy::transparent_production {
                static constexpr auto rule = LEXY_LIT(">=");
                static constexpr auto value = lexy::constant(ComparisonOp::ge);
            };
            struct gt : lexy::transparent_production {
                static constexpr auto rule = dsl::lit_c<'>'>;
                static constexpr auto value = lexy::constant(ComparisonOp::gt);
            };
            struct in_ : lexy::transparent_production {
                static constexpr auto rule = kw_in;
                static constexpr auto value = lexy::constant(ComparisonOp::in_);
            };

            // @note order matters: longer matches first (ne before eq, le before lt, ge before gt)
            static constexpr auto rule = dsl::p<ne> | dsl::p<le> | dsl::p<ge> | dsl::p<lt> | dsl::p<gt> | 
                                        dsl::p<eq> | dsl::p<in_>;
            static constexpr auto value = lexy::forward<ComparisonOp>;
        };

        // value for WHERE/INSERT/UPDATE
        struct term_value {
            static constexpr auto rule = dsl::p<string_literal> | dsl::p<integer_literal> | dsl::p<boolean_literal>;
            static constexpr auto value = lexy::construct<dtype::WriteValue>;
        };

        // WHERE relation: column op value
        struct where_relation {
            static constexpr auto rule = [] {
                return (dsl::member<&WhereRelation::column_name> = dsl::p<identifier>) + dsl::p<ws> +
                       (dsl::member<&WhereRelation::op> = dsl::p<comparison_op>) + dsl::p<ws> +
                       (dsl::member<&WhereRelation::value> = dsl::p<term_value>) + dsl::p<ws>;
            }();
            static constexpr auto value = lexy::as_aggregate<WhereRelation>;
        };

        struct unexpected_trailing_and_where {
            static constexpr auto name = "unexpected trailing AND in WHERE clause";
        };

        // WHERE clause
        struct where_clause {
            static constexpr auto rule = []() {
                auto relations = dsl::list(
                    dsl::p<where_relation>,
                    dsl::sep(kw_and >> dsl::p<ws>).trailing_error<unexpected_trailing_and_where>
                );
                return dsl::opt(kw_where >> dsl::p<ws> + relations);
            }();
            static constexpr auto value = lexy::as_list<CappedArray<WhereRelation, MAX_WHERE_RELATIONS>>;
        };

        // assignment: column = value
        struct assignment {
            static constexpr auto rule = [] {
                return (dsl::member<&Assignment::column_name> = dsl::p<identifier>) + dsl::p<ws> +
                       dsl::lit_c<'='> + dsl::p<ws> +
                       (dsl::member<&Assignment::value> = dsl::p<term_value>) + dsl::p<ws>;
            }();
            static constexpr auto value = lexy::as_aggregate<Assignment>;
        };

        struct unexpected_trailing_comma_assignments {
            static constexpr auto name = "unexpected trailing comma in SET clause";
        };

        // SET clause assignments
        struct set_assignments {
            static constexpr auto rule = [] {
                return dsl::list(
                    dsl::p<assignment>,
                    dsl::sep(dsl::lit_c<','> >> dsl::p<ws>).trailing_error<unexpected_trailing_comma_assignments>
                );
            }();
            static constexpr auto value = lexy::as_list<CappedArray<Assignment, MAX_ASSIGNMENTS>>;
        };

        // LIMIT clause
        struct limit_clause {
            static constexpr auto rule = []() {
                return dsl::opt(kw_limit >> dsl::p<ws> + dsl::p<integer_literal>);
            }();
            static constexpr auto value = lexy::callback<S64>(
                [](lexy::nullopt) { return -1; },
                [](S64 val) { return val; }
            );
        };

        // ORDER BY clause
        constexpr auto kw_order = LEXY_LIT_CI("order");
        constexpr auto kw_by = LEXY_LIT_CI("by");
        constexpr auto kw_asc = LEXY_LIT_CI("asc");
        constexpr auto kw_desc = LEXY_LIT_CI("desc");

        struct sort_order {
            struct asc : lexy::transparent_production {
                static constexpr auto rule = kw_asc;
                static constexpr auto value = lexy::constant(SortOrder::asc);
            };
            struct desc : lexy::transparent_production {
                static constexpr auto rule = kw_desc;
                static constexpr auto value = lexy::constant(SortOrder::desc);
            };

            // @note defaults to ascending
            static constexpr auto rule = dsl::opt(dsl::p<asc> | dsl::p<desc>);
            static constexpr auto value = lexy::callback<SortOrder>(
                [](lexy::nullopt) { return SortOrder::asc; },
                [](SortOrder o) { return o; }
            );
        };

        struct order_by_item {
            static constexpr auto rule = [] {
                return (dsl::member<&OrderByClause::column_name> = dsl::p<identifier>) + dsl::p<ws> +
                       (dsl::member<&OrderByClause::order> = dsl::p<sort_order>) + dsl::p<ws>;
            }();
            static constexpr auto value = lexy::as_aggregate<OrderByClause>;
        };

        struct order_by_clause {
            static constexpr auto rule = []() {
                auto items = dsl::list(
                    dsl::p<order_by_item>,
                    dsl::sep(dsl::lit_c<','> >> dsl::p<ws>)
                );
                return dsl::opt(kw_order >> dsl::p<ws> + kw_by + dsl::p<ws> + items);
            }();
            static constexpr auto value = lexy::as_list<CappedArray<OrderByClause, MAX_ORDER_BY>>;
        };

        // ALLOW FILTERING clause
        constexpr auto kw_allow = LEXY_LIT_CI("allow");
        constexpr auto kw_filtering = LEXY_LIT_CI("filtering");

        struct allow_filtering_clause {
            static constexpr auto rule = []() {
                auto key = kw_allow + dsl::p<ws> + kw_filtering;
                return dsl::opt(dsl::peek(key) >> key);
            }();
            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };

        // GROUP BY clause
        constexpr auto kw_group = LEXY_LIT_CI("group");

        struct group_by_clause {
            static constexpr auto rule = []() {
                auto columns = dsl::list(dsl::p<identifier>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
                return dsl::opt(kw_group >> dsl::p<ws> + kw_by + dsl::p<ws> + columns);
            }();
            static constexpr auto value = lexy::as_list<CappedArray<String8, MAX_GROUP_BY>>;
        };

        // USING TIMESTAMP
        struct using_timestamp {
            static constexpr auto rule = []() {
                return dsl::opt(kw_using >> dsl::p<ws> + kw_timestamp + dsl::p<ws> + dsl::p<integer_literal>);
            }();
            static constexpr auto value = lexy::callback<S64>(
                [](lexy::nullopt) { return -1; },
                [](S64 val) { return val; }
            );
        };

        // USING TTL
        struct using_ttl {
            static constexpr auto rule = []() {
                return dsl::opt(kw_using >> dsl::p<ws> + kw_ttl + dsl::p<ws> + dsl::p<integer_literal>);
            }();
            static constexpr auto value = lexy::callback<S64>(
                [](lexy::nullopt) { return -1; },
                [](S64 val) { return val; }
            );
        };

        // USING TIMESTAMP/TTL
        struct using_options {
            static constexpr auto rule = []() {
                auto timestamp_opt = dsl::peek(kw_timestamp) >> kw_timestamp + dsl::p<ws> + dsl::p<integer_literal>;
                auto ttl_opt = dsl::peek(kw_ttl) >> kw_ttl + dsl::p<ws> + dsl::p<integer_literal>;
                auto first = timestamp_opt | ttl_opt;
                auto second = dsl::opt(kw_and >> dsl::p<ws> + (timestamp_opt | ttl_opt));
                return dsl::opt(kw_using >> dsl::p<ws> + first + dsl::p<ws> + second);
            }();
            // @note returns pair of (timestamp, ttl) - both -1 if not specified
            static constexpr auto value = lexy::callback<Pair<S64, S64>>(
                [](lexy::nullopt) { return Pair<S64, S64>{-1, -1}; },
                [](S64 first) { return Pair<S64, S64>{first, -1}; },
                [](S64 first, lexy::nullopt) { return Pair<S64, S64>{first, -1}; },
                [](S64 first, S64 second) { return Pair<S64, S64>{first, second}; }
            );
        };

        // Map literal for options like replication = {'class': 'SimpleStrategy'}
        struct option_value {
            static constexpr auto rule = dsl::p<string_literal> | dsl::p<integer_literal> | dsl::p<boolean_literal>;
            static constexpr auto value = lexy::callback<OptionValue>(
                [](AutoString8&& s) { return OptionValue{move(s)}; },
                [](S64 val) { return OptionValue{val}; },
                [](bool val) { return OptionValue{val}; }
            );
        };

        struct map_entry {
            static constexpr auto rule = [] {
                auto key = dsl::p<string_literal>;
                auto colon = dsl::lit_c<':'>;
                auto val = dsl::p<option_value>;
                return key + dsl::p<ws> + colon + dsl::p<ws> + val;
            }();
            static constexpr auto value = lexy::callback<Pair<AutoString8, OptionValue>>(
                [](AutoString8&& k, OptionValue&& v) { return Pair<AutoString8, OptionValue>{move(k), move(v)}; }
            );
        };

        struct map_literal {
            static constexpr auto rule = [] {
                auto entries = dsl::list(dsl::p<map_entry>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
                return dsl::curly_bracketed(dsl::p<ws> + entries + dsl::p<ws>);
            }();
            static constexpr auto value = lexy::as_list<MapLiteral>;
        };

        struct keyspace_option_value {
            static constexpr auto rule = dsl::p<map_literal> | dsl::p<integer_literal> | dsl::p<string_literal>;
            static constexpr auto value = lexy::callback<KeyspaceOptionValue>(
                [](MapLiteral&& m) { return KeyspaceOptionValue{move(m)}; },
                [](S64 val) { return KeyspaceOptionValue{val}; },
                [](AutoString8&& s) { return KeyspaceOptionValue{move(s)}; }
            );
        };
    
        struct keyspace_option {
            static constexpr auto rule = [] {
                auto key = (dsl::member<&KeyspaceOption::key> = dsl::p<identifier>);
                auto eq = dsl::lit_c<'='>;
                auto value = (dsl::member<&KeyspaceOption::value> = dsl::p<keyspace_option_value>);
                
                return key + dsl::p<ws> + eq + dsl::p<ws> + value + dsl::p<ws>;
            }();
            
            static constexpr auto value = lexy::as_aggregate<KeyspaceOption>;
        };

        struct unexpected_trailing_and {
            static constexpr auto name = "unexpected trailing AND";
        };
    
        struct with_options {
            static constexpr auto rule = []() {
                auto options = dsl::list(
                    dsl::recurse<keyspace_option> + dsl::p<ws>,
                    dsl::sep(kw_and >> dsl::p<ws>).trailing_error<unexpected_trailing_and>
                );
                return dsl::opt(kw_with >> dsl::p<ws> + options);
            }();
            
            static constexpr auto value = lexy::as_list<CappedArray<KeyspaceOption, MAX_KEYSPACE_OPTIONS>>;
        };

        // ====================================================================
        // USE statement
        // ====================================================================
        struct use_stmt {
            static constexpr auto rule = [] {
                return dsl::peek(kw_use) >> kw_use + dsl::p<ws> +
                    (dsl::member<&UseKeyspace::keyspace_name> = dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::as_aggregate<UseKeyspace>;
        };
    
        // ====================================================================
        // CREATE KEYSPACE statement
        // ====================================================================
        struct create_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_create + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&CreateKeyspace::if_not_exists> = dsl::p<if_not_exists>) + dsl::p<ws> +
                    (dsl::member<&CreateKeyspace::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&CreateKeyspace::options> = dsl::p<with_options>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateKeyspace>;
        };

        // ====================================================================
        // ALTER KEYSPACE statement
        // ====================================================================
        struct alter_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_alter + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&AlterKeyspace::if_exists> = dsl::p<if_exists>) + dsl::p<ws> +
                    (dsl::member<&AlterKeyspace::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&AlterKeyspace::options> = dsl::p<with_options>);
            }();
            static constexpr auto value = lexy::as_aggregate<AlterKeyspace>;
        };

        // ====================================================================
        // DROP KEYSPACE statement
        // ====================================================================
        struct drop_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_drop + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&DropKeyspace::if_exists> = dsl::p<if_exists>) + dsl::p<ws> +
                    (dsl::member<&DropKeyspace::keyspace_name> = dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::as_aggregate<DropKeyspace>;
        };

        // ====================================================================
        // CREATE TABLE statement
        // ====================================================================
        struct primary_key {
            static constexpr auto rule = [] {
                auto key = kw_primary + dsl::p<ws> + kw_key;
                return dsl::opt(dsl::peek(key) >> key);
            }();

            static constexpr auto value = lexy::callback<bool>(
                [](lexy::nullopt) { return false; },
                []() { return true; }
            );
        };
    
        struct column_def {
            static constexpr auto rule = [] {
                return (dsl::member<&CreateColumn::name> = dsl::p<identifier>) + dsl::p<ws> + 
                       (dsl::member<&CreateColumn::dtype> = dsl::p<data_type>) + dsl::p<ws> + 
                       (dsl::member<&CreateColumn::is_primary_key> = dsl::p<primary_key>) + dsl::p<ws>;
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateColumn>;
        };

        struct column_list {
            static constexpr auto rule = [] {
                return dsl::parenthesized(
                    dsl::p<ws> + dsl::list(dsl::p<column_def>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>))
                );
            }();

            static constexpr auto value = lexy::as_list<DynamicArray<CreateColumn>>;
        };

        struct create_table_stmt {
            static constexpr auto rule = [] {                
                auto key = kw_create + dsl::p<ws> + (kw_table | kw_columnfamily);
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&CreateTable::if_not_exists> = dsl::p<if_not_exists>) + dsl::p<ws> +
                    (dsl::member<&CreateTable::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                    (dsl::member<&CreateTable::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&CreateTable::columns> = dsl::p<column_list>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateTable>;
        };

        // ====================================================================
        // DROP TABLE statement
        // ====================================================================
        struct drop_table_stmt {
            static constexpr auto rule = [] {
                auto key = kw_drop + dsl::p<ws> + (kw_table | kw_columnfamily);
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&DropTable::if_exists> = dsl::p<if_exists>) + dsl::p<ws> +
                    (dsl::member<&DropTable::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                    (dsl::member<&DropTable::table_name> = dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::as_aggregate<DropTable>;
        };

        // ====================================================================
        // TRUNCATE statement
        // ====================================================================
        struct truncate_table_keyword {
            static constexpr auto rule = dsl::opt(dsl::peek(kw_table | kw_columnfamily) >> (kw_table | kw_columnfamily) + dsl::p<ws>);
            static constexpr auto value = lexy::noop;
        };

        struct truncate_stmt {
            static constexpr auto rule = [] {
                auto key = kw_truncate;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::p<truncate_table_keyword> +
                    (dsl::member<&TruncateTable::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                    (dsl::member<&TruncateTable::table_name> = dsl::p<identifier>);
            }();
            static constexpr auto value = lexy::as_aggregate<TruncateTable>;
        };

        // ====================================================================
        // INSERT INTO statement
        // ====================================================================
        struct insert_value {
            static constexpr auto rule = dsl::p<string_literal> | dsl::p<integer_literal>;
            static constexpr auto value = lexy::construct<dtype::WriteValue>;
        };

        struct values_list {
            static constexpr auto rule = [] {
                return dsl::parenthesized(
                    dsl::p<ws> + dsl::list(dsl::p<insert_value>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>))
                );
            }();

            static constexpr auto value = lexy::as_list<DynamicArray<dtype::WriteValue>>;
        };
    
        struct column_names_list {
            static constexpr auto rule = [] {
                auto column_list = dsl::parenthesized(
                    dsl::p<ws> + dsl::list(dsl::p<identifier>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>)) + dsl::p<ws>
                );
                return dsl::opt(column_list + dsl::p<ws>);
            }();
            
            static constexpr auto value = lexy::as_list<DynamicArray<String8>>;
        };

        struct insert_into_stmt {
            static constexpr auto rule = [] {
                auto key = kw_insert + dsl::p<ws> + kw_into;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&InsertInto::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> + dsl::lit_c<'.'> +
                    (dsl::member<&InsertInto::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&InsertInto::column_names> = dsl::p<column_names_list>) + dsl::p<ws> +
                    kw_values + dsl::p<ws> +
                    (dsl::member<&InsertInto::values> = dsl::p<values_list>) + dsl::p<ws> +
                    (dsl::member<&InsertInto::if_not_exists> = dsl::p<if_not_exists>) + dsl::p<ws> +
                    (dsl::member<&InsertInto::timestamp> = dsl::p<using_timestamp>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<InsertInto>;
        };

        // ====================================================================
        // UPDATE statement
        // ====================================================================
        struct update_stmt {
            static constexpr auto rule = [] {
                auto key = kw_update;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&Update::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                    (dsl::member<&Update::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&Update::timestamp> = dsl::p<using_timestamp>) + dsl::p<ws> +
                    kw_set + dsl::p<ws> +
                    (dsl::member<&Update::assignments> = dsl::p<set_assignments>) + dsl::p<ws> +
                    (dsl::member<&Update::where> = dsl::p<where_clause>) + dsl::p<ws> +
                    (dsl::member<&Update::if_exists> = dsl::p<if_exists>);
            }();
            static constexpr auto value = lexy::as_aggregate<Update>;
        };

        // ====================================================================
        // DELETE statement
        // ====================================================================
        struct delete_column_list {
            static constexpr auto rule = [] {
                auto columns = dsl::list(dsl::p<identifier>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
                return dsl::opt(dsl::peek_not(kw_from) >> columns);
            }();
            static constexpr auto value = lexy::as_list<CappedArray<String8, MAX_COLUMN_NAMES>>;
        };

        struct delete_stmt {
            static constexpr auto rule = [] {
                auto key = kw_delete;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&Delete::column_names> = dsl::p<delete_column_list>) + dsl::p<ws> +
                    kw_from + dsl::p<ws> +
                    (dsl::member<&Delete::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                    (dsl::member<&Delete::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&Delete::timestamp> = dsl::p<using_timestamp>) + dsl::p<ws> +
                    (dsl::member<&Delete::where> = dsl::p<where_clause>) + dsl::p<ws> +
                    (dsl::member<&Delete::if_exists> = dsl::p<if_exists>);
            }();
            static constexpr auto value = lexy::as_aggregate<Delete>;
        };

        // ====================================================================
        // SELECT FROM statement
        // ====================================================================
        struct select_columns {
            struct star : lexy::transparent_production {
                static constexpr auto rule = dsl::lit_c<'*'>;
                static constexpr auto value = lexy::constant(CappedArray<String8, MAX_COLUMN_NAMES>{});
            };
            struct column_names : lexy::transparent_production {
                static constexpr auto rule = dsl::list(dsl::p<identifier>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>));
                static constexpr auto value = lexy::as_list<CappedArray<String8, MAX_COLUMN_NAMES>>;
            };

            static constexpr auto rule = dsl::p<star> | dsl::p<column_names>;
            static constexpr auto value = lexy::forward<CappedArray<String8, MAX_COLUMN_NAMES>>;
        };

        struct select_from_stmt {
            static constexpr auto rule = [] {
                auto key = kw_select;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&SelectFrom::column_names> = dsl::p<select_columns>) + dsl::p<ws> + 
                    kw_from + dsl::p<ws> +
                    (dsl::member<&SelectFrom::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> + dsl::lit_c<'.'> + 
                    (dsl::member<&SelectFrom::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&SelectFrom::where> = dsl::p<where_clause>) + dsl::p<ws> +
                    (dsl::member<&SelectFrom::group_by> = dsl::p<group_by_clause>) + dsl::p<ws> +
                    (dsl::member<&SelectFrom::order_by> = dsl::p<order_by_clause>) + dsl::p<ws> +
                    (dsl::member<&SelectFrom::limit> = dsl::p<limit_clause>) + dsl::p<ws> +
                    (dsl::member<&SelectFrom::allow_filtering> = dsl::p<allow_filtering_clause>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<SelectFrom>;
        };
    
        struct cql_statement {
            static constexpr auto rule = [] {
                auto use = dsl::p<use_stmt>;
                auto create_ks = dsl::p<create_keyspace_stmt>;
                auto alter_ks = dsl::p<alter_keyspace_stmt>;
                auto drop_ks = dsl::p<drop_keyspace_stmt>;
                auto create_tbl = dsl::p<create_table_stmt>;
                auto drop_tbl = dsl::p<drop_table_stmt>;
                auto truncate = dsl::p<truncate_stmt>;
                auto insert = dsl::p<insert_into_stmt>;
                auto update = dsl::p<update_stmt>;
                auto del = dsl::p<delete_stmt>;
                auto select = dsl::p<select_from_stmt>;
                
                return dsl::p<ws> + (use | create_ks | alter_ks | drop_ks | create_tbl | drop_tbl | 
                                     truncate | insert | update | del | select) + dsl::p<ws> + dsl::lit_c<';'>;
            }();
            
            static constexpr auto value = lexy::construct<Statement>;
        };

        struct query_complete_scanner {
            static constexpr auto rule = dsl::until(dsl::lit_c<';'>);
            static constexpr auto value = lexy::noop;
        };
    }

    struct LogErrorCallback {
        struct _sink {
            using return_type = std::size_t;
            std::size_t _count = 0;

            template <typename Input, typename Reader, typename Tag>
            void operator()(const lexy::error_context<Input>& context,
                            const lexy::error<Reader, Tag>& error) {
                auto location = lexy::get_input_location(context.input(), error.position());

                AutoString8 msg;
                if constexpr (std::is_same_v<Tag, lexy::expected_literal>) {
                    msg = fmt("error: while parsing %s at %u:%u: expected '%.*s'",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              static_cast<int>(error.length()), reinterpret_cast<const char*>(error.string()));
                } else if constexpr (std::is_same_v<Tag, lexy::expected_keyword>) {
                    msg = fmt("error: while parsing %s at %u:%u: expected keyword '%.*s'",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              static_cast<int>(error.length()), reinterpret_cast<const char*>(error.string()));
                } else if constexpr (std::is_same_v<Tag, lexy::expected_char_class>) {
                    msg = fmt("error: while parsing %s at %u:%u: expected %s",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              error.name());
                } else {
                    msg = fmt("error: while parsing %s at %u:%u: %s",
                              context.production(),
                              location.line_nr(), location.column_nr(),
                              error.message());
                }

                log::cql_parse_error(msg.c_str, msg.length);
                ++_count;
            }

            std::size_t finish() && {
                return _count;
            }
        };

        auto sink() const {
            return _sink{};
        }
    };

    Optional<Statement> parse(String8 bytes, bool report_errors) {
        log::cql_parse(bytes);

        auto input = lexy::string_input<lexy::ascii_encoding>(bytes.data, bytes.length);

        if (report_errors) {
            auto result = lexy::parse<grammar::cql_statement>(input, LogErrorCallback{});
            if (result.has_value()) {
                return result.value();
            }
        } else {
            auto result = lexy::parse<grammar::cql_statement>(input, lexy::noop);
            if (result.has_value()) {
                return result.value();
            }
        }
        return {};
    }

    bool is_complete(String8 bytes) {
        auto input = lexy::string_input<lexy::ascii_encoding>(bytes.data, bytes.length);
        auto result = lexy::parse<grammar::query_complete_scanner>(input, lexy::noop);
        return result.is_success();
    }
}