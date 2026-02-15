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

import objstore.dtypes;

namespace objstore::parser {
    // ========================================================================
    // http
    // ========================================================================
    struct ParserState {
        HttpRequest* out;
        bool message_complete = false;
        String8 current_header_name{};
        bool parsing_value = false;
    };

    static int on_url(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
        state->out->url = {reinterpret_cast<const uint8_t*>(at), length};
        return 0;
    }

    static int on_header_field(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
        if (state->parsing_value) {
            if (state->out->header_count < MAX_HEADERS) {
                state->out->headers.values[state->out->header_count - 1].name = state->current_header_name;
            }
            state->current_header_name = {};
            state->parsing_value = false;
        }
        state->current_header_name = {reinterpret_cast<const uint8_t*>(at), length};
        return 0;
    }

    static int on_header_value(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
        if (state->out->header_count < MAX_HEADERS) {
            state->out->headers.values[state->out->header_count].name = state->current_header_name;
            state->out->headers.values[state->out->header_count].value = {reinterpret_cast<const uint8_t*>(at), length};
            state->out->header_count++;
        }
        state->parsing_value = true;
        return 0;
    }

    static int on_body(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
        state->out->body = {reinterpret_cast<const uint8_t*>(at), length};
        return 0;
    }

    static int on_message_complete(llhttp_t* parser) {
        auto* state = static_cast<ParserState*>(parser->data);
        state->message_complete = true;
        return 0;
    }

    static int on_method(llhttp_t* parser, const char* at, size_t length) {
        auto* state = static_cast<ParserState*>(parser->data);
        state->out->method = {reinterpret_cast<const uint8_t*>(at), length};
        return 0;
    }

    llhttp_settings_t& get_settings() {
        static llhttp_settings_t settings;
        static bool initialized = false;
        if (!initialized) {
            llhttp_settings_init(&settings);
            settings.on_url = on_url;
            settings.on_header_field = on_header_field;
            settings.on_header_value = on_header_value;
            settings.on_body = on_body;
            settings.on_message_complete = on_message_complete;
            settings.on_method = on_method;
            initialized = true;
        }
        return settings;
    }

    llhttp_t& get_parser_template() {
        static llhttp_t parser_template;
        static bool initialized = false;
        if (!initialized) {
            llhttp_init(&parser_template, HTTP_REQUEST, &get_settings());
            initialized = true;
        }
        return parser_template;
    }

    Optional<HttpRequest> parse_request(String8 bytes) {
        llhttp_t parser = get_parser_template();

        HttpRequest result;

        ParserState state{};
        state.out = &result;
        parser.data = &state;

        llhttp_errno err = llhttp_execute(&parser, bytes.data, bytes.length);

        if (err != HPE_OK || !state.message_complete) {
            return {};
        }
        return Optional{result};
    }

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
            struct text : lexy::transparent_production
            {
                static constexpr auto rule  = LEXY_LIT_CI("text");
                static constexpr auto value = lexy::constant(DType::Text);
            };
            struct int_ : lexy::transparent_production
            {
                static constexpr auto rule  = LEXY_LIT_CI("int");
                static constexpr auto value = lexy::constant(DType::Int);
            };

            static constexpr auto rule  = dsl::p<text> | dsl::p<int_>;
            static constexpr auto value = lexy::forward<DType>;
        };
    
        // string literal
        struct string_literal {
            static constexpr auto escaped_symbols = lexy::symbol_table<char>
                                                        .map<'"'>('"')
                                                        .map<'\\'>('\\')
                                                        .map<'/'>('/')
                                                        .map<'b'>('\b')
                                                        .map<'f'>('\f')
                                                        .map<'n'>('\n')
                                                        .map<'r'>('\r')
                                                        .map<'t'>('\t');
                                                        
            static constexpr auto rule = [] {
                auto escape = dsl::backslash_escape.symbol<escaped_symbols>();    
                return dsl::single_quoted(-dsl::ascii::control, escape);
            }();
            
            // @todo arena allocator, this needs allocation because to resolve escape characters
            static constexpr auto value = lexy::as_string<STLString>;
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
                auto key = (dsl::member<&CreateKeyspaceRequestOption::key> = dsl::p<identifier>);
                auto eq = dsl::lit_c<'='>;
                auto value = (dsl::member<&CreateKeyspaceRequestOption::value> = (dsl::p<integer_literal> | dsl::p<string_literal>));
                
                return key + dsl::p<ws> + eq + dsl::p<ws> + value + dsl::p<ws>;
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateKeyspaceRequestOption>;
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
            
            static constexpr auto value = lexy::as_list<STLArray<CreateKeyspaceRequestOption, MAX_KEYSPACE_OPTIONS>>;
        };
    
        // CREATE KEYSPACE statement
        struct create_keyspace_stmt {
            static constexpr auto rule = [] {
                auto key = kw_create + dsl::p<ws> + kw_keyspace;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&CreateKeyspaceRequest::if_not_exists> = dsl::p<if_not_exists>) + dsl::p<ws> +
                    (dsl::member<&CreateKeyspaceRequest::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&CreateKeyspaceRequest::options> = dsl::p<with_options>) + dsl::eof;
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateKeyspaceRequest>;
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
                return (dsl::member<&CreateTableRequestColumn::name> = dsl::p<identifier>) + dsl::p<ws> + 
                       (dsl::member<&CreateTableRequestColumn::dtype> = dsl::p<data_type>) + dsl::p<ws> + 
                       (dsl::member<&CreateTableRequestColumn::is_primary_key> = dsl::p<primary_key>) + dsl::p<ws>;
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateTableRequestColumn>;
        };

        struct column_list {
            static constexpr auto rule = [] {
                return dsl::parenthesized(
                    dsl::list(dsl::p<column_def>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>))
                );
            }();

            static constexpr auto value = lexy::as_list<STLDynamicArray<CreateTableRequestColumn>>;
        };

        // CREATE TABLE statement
        struct create_table_stmt {
            static constexpr auto rule = [] {                
                auto key = kw_create + dsl::p<ws> + kw_table;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&CreateTableRequest::if_not_exists> = dsl::p<if_not_exists>) + dsl::p<ws> +
                    (dsl::member<&CreateTableRequest::table_name> = dsl::p<identifier>) + dsl::p<ws> +
                    (dsl::member<&CreateTableRequest::columns> = dsl::p<column_list>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<CreateTableRequest>;
        };
    
        // value for INSERT statement
        struct insert_value {
            static constexpr auto rule = dsl::p<string_literal> | dsl::p<integer_literal>;
            
            static constexpr auto value = lexy::callback<InsertValue>(
                [](STLString str) {
                    InsertValue val;
                    val.dtype = DType::Text;
                    val.value = str;
                    return val;
                },
                [](S64 num) {
                    InsertValue val;
                    val.dtype = DType::Int;
                    val.value = num;
                    return val;
                }
            );
        };

        struct values_list {
            static constexpr auto rule = [] {
                return dsl::parenthesized(
                    dsl::list(dsl::p<insert_value>, dsl::sep(dsl::lit_c<','> >> dsl::p<ws>))
                );
            }();

            static constexpr auto value = lexy::as_list<STLDynamicArray<InsertValue>>;
        };
    
        // INSERT INTO statement
        struct insert_into_stmt {
            static constexpr auto rule = [] {
                auto key = kw_insert + dsl::p<ws> + kw_into;
                return dsl::peek(key) >> key + dsl::p<ws> +
                    (dsl::member<&InsertIntoRequest::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> + dsl::lit_c<'.'> +
                    (dsl::member<&InsertIntoRequest::table_name> = dsl::p<identifier>) + dsl::p<ws> + kw_values + dsl::p<ws> +
                    (dsl::member<&InsertIntoRequest::values> = dsl::p<values_list>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<InsertIntoRequest>;
        };
    
        // SELECT FROM statement
        struct select_from_stmt {
            static constexpr auto rule = [] {
                // @todo
                auto key = kw_select;
                return dsl::peek(key) >> key + dsl::lit_c<'*'> + dsl::p<ws> + kw_from + dsl::p<ws> +
                    (dsl::member<&SelectFromRequest::keyspace_name> = dsl::p<identifier>) + dsl::p<ws> + dsl::lit_c<'.'> + 
                    (dsl::member<&SelectFromRequest::table_name> = dsl::p<identifier>);
            }();
            
            static constexpr auto value = lexy::as_aggregate<SelectFromRequest>;
        };
    
        struct cql_statement {
            static constexpr auto rule = [] {
                auto create_ks = dsl::p<create_keyspace_stmt>;
                auto create_tbl = dsl::p<create_table_stmt>;
                auto insert = dsl::p<insert_into_stmt>;
                auto select = dsl::p<select_from_stmt>;
                
                return dsl::p<ws> + (create_ks | create_tbl | insert | select) + dsl::p<ws> + (dsl::lit_c<';'> | dsl::eof);
            }();
            
            static constexpr auto value = lexy::construct<CqlRequest>;
        };
    }

    // @todo return error message
    Optional<CqlRequest> parse_cql(String8 bytes) {
        auto input = lexy::string_input<lexy::ascii_encoding>(bytes.data, bytes.length);
        
        auto result = lexy::parse<grammar::cql_statement>(input, lexy_ext::report_error.path("cql"));

        if (result.has_value()) {
            return result.value();
        }
        return {};
    }
}