module;
#include <llhttp.h>

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

// @todo
#include <lexy_ext/report_error.hpp>

#define LEXY_LIT_CI(x) lexy::dsl::ascii::case_folding(LEXY_LIT(x))

module objstore.parser;

import plexdb.base;
import plexdb.os;
import plexdb.os.containers;

namespace objstore::parser::http {
    // ========================================================================
    // incremental parser
    // ========================================================================
    // @note accumulates data since llhttp callbacks point into current input buffer only
    struct ParserState {
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
        auto* state = static_cast<ParserState*>(parser->data);
        U64 copy_len = min(length, MAX_METHOD_SIZE - state->method_len);
        os::memory_copy(state->method_buf + state->method_len, at, copy_len);
        state->method_len += copy_len;
        return 0;
    }

    static int inc_on_method_complete(llhttp_t* parser) {
        auto* state = static_cast<ParserState*>(parser->data);
        state->request.method = {reinterpret_cast<const U8*>(state->method_buf), state->method_len};
        return 0;
    }

    static int inc_on_url(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
        U64 copy_len = min(length, MAX_URL_SIZE - state->url_len);
        os::memory_copy(state->url_buf + state->url_len, at, copy_len);
        state->url_len += copy_len;
        return 0;
    }

    static int inc_on_url_complete(llhttp_t* parser) {
        auto* state = static_cast<ParserState*>(parser->data);
        state->request.url = {reinterpret_cast<const U8*>(state->url_buf), state->url_len};
        return 0;
    }

    static int inc_on_header_field(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
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
        auto* state = static_cast<ParserState*>(parser->data);
        state->in_header_value = true;
        
        U64 copy_len = min(length, MAX_HEADER_VALUE_SIZE - state->header_value_len);
        os::memory_copy(state->header_value_buf + state->header_value_len, at, copy_len);
        state->header_value_len += copy_len;
        return 0;
    }

    static int inc_on_headers_complete(llhttp_t* parser) {
        auto* state = static_cast<ParserState*>(parser->data);
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
        auto* state = static_cast<ParserState*>(parser->data);
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
        auto* state = static_cast<ParserState*>(parser->data);
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

    ParserState* parser_create() {
        auto* state = reinterpret_cast<ParserState*>(os::allocate_zero(sizeof(ParserState)));
        llhttp_init(&state->parser, HTTP_REQUEST, &get_incremental_settings());
        state->parser.data = state;
        return state;
    }

    void parser_reset(ParserState* state) {
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

    void parser_destroy(ParserState* state) {
        if (state) {
            if (state->body_buf) {
                os::deallocate(state->body_buf);
            }
            os::deallocate(state);
        }
    }

    void parser_execute(ParserState* state, const char* data, U64 length) {
        if (state->has_error || state->message_complete) {
            return;
        }
        
        llhttp_errno err = llhttp_execute(&state->parser, data, length);
        
        if (err != HPE_OK) {
            state->has_error = true;
        }
    }

    const Request& parser_get_request(const ParserState* state) {
        return state->request;
    }

    bool parser_is_complete(const ParserState* state) {
        return state->message_complete;
    }

    bool parser_has_error(const ParserState* state) {
        return state->has_error;
    }
}

namespace objstore::parser::cql {
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
        constexpr auto kw_keyspace = LEXY_LIT_CI("keyspace");
        constexpr auto kw_table = LEXY_LIT_CI("table");
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
        // @todo compliance
        // https://cassandra.apache.org/doc/latest/cassandra/developing/cql/cql_singlefile.html#statements
        // https://docs.datastax.com/en/cql-oss/3.3/cql/cql_reference/escape_char_r.html
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
    
        // WITH options for CREATE KEYSPACE
        struct keyspace_option {
            static constexpr auto rule = [] {
                auto key = (dsl::member<&CreateKeyspaceOption::key> = dsl::p<identifier>);
                auto eq = dsl::lit_c<'='>;
                auto value = (dsl::member<&CreateKeyspaceOption::value> = (dsl::p<integer_literal> | dsl::p<string_literal>));
                
                return key + dsl::p<ws> + eq + dsl::p<ws> + value + dsl::p<ws>;
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateKeyspaceOption>;
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
            
            static constexpr auto value = lexy::as_list<CappedArray<CreateKeyspaceOption, MAX_KEYSPACE_OPTIONS>>;
        };
    
        // CREATE KEYSPACE statement
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
    
        // column definition for CREATE TABLE
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

        // CREATE TABLE statement
        struct create_table_stmt {
            static constexpr auto rule = [] {                
                auto key = kw_create + dsl::p<ws> + kw_table;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&CreateTable::if_not_exists> = dsl::p<if_not_exists>) + dsl::p<ws> +
                    (dsl::member<&CreateTable::keyspace_name> = dsl::p<identifier>) + dsl::lit_c<'.'> +
                    (dsl::member<&CreateTable::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&CreateTable::columns> = dsl::p<column_list>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateTable>;
        };
    
        // value for INSERT statement
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
    
        // INSERT INTO statement
        struct insert_into_stmt {
            static constexpr auto rule = [] {
                auto key = kw_insert + dsl::p<ws> + kw_into;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&InsertInto::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> + dsl::lit_c<'.'> +
                    (dsl::member<&InsertInto::table_name> = dsl::p<identifier>) + dsl::p<ws> + kw_values + dsl::p<ws> +
                    (dsl::member<&InsertInto::values> = dsl::p<values_list>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<InsertInto>;
        };
    
        // SELECT FROM statement
        struct select_from_stmt {
            static constexpr auto rule = [] {
                // @todo
                auto key = kw_select;
                return dsl::peek(key) >> key + dsl::p<ws> + dsl::lit_c<'*'> + dsl::p<ws> + kw_from + dsl::p<ws> +
                    (dsl::member<&SelectFrom::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> + dsl::lit_c<'.'> + 
                    (dsl::member<&SelectFrom::table_name> = dsl::p<identifier>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<SelectFrom>;
        };
    
        struct cql_statement {
            static constexpr auto rule = [] {
                auto create_ks = dsl::p<create_keyspace_stmt>;
                auto create_tbl = dsl::p<create_table_stmt>;
                auto insert = dsl::p<insert_into_stmt>;
                auto select = dsl::p<select_from_stmt>;
                
                return dsl::p<ws> + (create_ks | create_tbl | insert | select) + dsl::p<ws> + dsl::lit_c<';'>;
            }();
            
            static constexpr auto value = lexy::construct<Statement>;
        };

        struct query_complete_scanner {
            static constexpr auto rule = dsl::until(dsl::lit_c<';'>);
            static constexpr auto value = lexy::noop;
        };
    }

    // @todo return error message
    Optional<Statement> parse(String8 bytes, bool report_errors) {
        auto input = lexy::string_input<lexy::ascii_encoding>(bytes.data, bytes.length);

        // auto reporter = lexy_ext::report_error.path("cql");
        auto reporter = lexy::noop;
        auto result = lexy::parse<grammar::cql_statement>(input, reporter);

        if (result.has_value()) {
            return result.value();
        }
        return {};
    }

    bool is_complete(String8 bytes) {
        auto input = lexy::string_input<lexy::ascii_encoding>(bytes.data, bytes.length);
        auto result = lexy::parse<grammar::query_complete_scanner>(input, lexy::noop);
        return result.is_success();
    }
}