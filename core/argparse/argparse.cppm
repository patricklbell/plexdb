export module plexdb.argparse;

import plexdb.base;

export namespace plexdb::argparse {
    constexpr U64 MAX_POSITIONAL = 16;
    constexpr U64 MAX_FLAGS      = 32;
    constexpr U64 MAX_NAME_LEN   = 64;
    constexpr U64 MAX_DESC_LEN   = 256;
    constexpr U64 MAX_VALUE_LEN  = 512;

    struct PositionalDef {
        char name[MAX_NAME_LEN];
        char description[MAX_DESC_LEN];
        bool optional;
    };

    constexpr U64 MAX_OPTIONS = 16;

    struct FlagDef {
        char long_name[MAX_NAME_LEN];
        char short_name[8];
        char description[MAX_DESC_LEN];
    };

    struct OptionDef {
        char long_name[MAX_NAME_LEN];
        char short_name[8];
        char description[MAX_DESC_LEN];
        char default_value[MAX_VALUE_LEN];
    };

    struct Parser {
        char                                       prog_name[MAX_NAME_LEN];
        char                                       description[MAX_DESC_LEN];
        CappedArray<PositionalDef, MAX_POSITIONAL> positionals;
        CappedArray<FlagDef, MAX_FLAGS>            flags;
        CappedArray<OptionDef, MAX_OPTIONS>        options;
    };

    struct ParseResult {
        bool ok;
        bool help_requested;
        char error[MAX_DESC_LEN];
        char positional_values[MAX_POSITIONAL][MAX_VALUE_LEN];
        U64  positional_count;
        U32  flag_bits;
        char option_values[MAX_OPTIONS][MAX_VALUE_LEN];
    };

    Parser create_parser(const char* prog_name, const char* description = "");
    U64    add_positional(Parser& parser, const char* name, const char* description = "");
    U64    add_optional_positional(Parser& parser, const char* name, const char* description = "");
    U64    add_flag(Parser& parser, const char* long_name, const char* short_name = "", const char* description = "");
    U64    add_option(Parser& parser, const char* long_name, const char* short_name = "", const char* description = "", const char* default_value = "");

    ParseResult parse(const Parser& parser, int argc, char* argv[]);
    void        print_help(const Parser& parser);

    String8 get_positional(const ParseResult& result, U64 index);
    String8 get_optional_positional(const ParseResult& result, U64 index);
    bool    has_flag(const ParseResult& result, U64 flag_index);
    String8 get_option(const ParseResult& result, U64 option_index);
}
