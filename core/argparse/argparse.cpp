module;
#include <string.h>
#include <stdio.h>
#include <plexdb/macros/macros.h>

module plexdb.argparse;

namespace plexdb::argparse {
    Parser create_parser(const char* prog_name, const char* description) {
        Parser p{};
        snprintf(p.prog_name,   MAX_NAME_LEN, "%s", prog_name);
        snprintf(p.description, MAX_DESC_LEN, "%s", description);
        return p;
    }

    U64 add_positional(Parser& parser, const char* name, const char* description) {
        PositionalDef def{};
        snprintf(def.name,        MAX_NAME_LEN, "%s", name);
        snprintf(def.description, MAX_DESC_LEN, "%s", description);
        def.optional = false;
        push_back(parser.positionals, def);
        return parser.positionals.cap - 1;
    }

    U64 add_optional_positional(Parser& parser, const char* name, const char* description) {
        PositionalDef def{};
        snprintf(def.name,        MAX_NAME_LEN, "%s", name);
        snprintf(def.description, MAX_DESC_LEN, "%s", description);
        def.optional = true;
        push_back(parser.positionals, def);
        return parser.positionals.cap - 1;
    }

    U64 add_flag(Parser& parser, const char* long_name, const char* short_name, const char* description) {
        FlagDef def{};
        snprintf(def.long_name,   MAX_NAME_LEN, "%s", long_name);
        snprintf(def.short_name,  8,            "%s", short_name);
        snprintf(def.description, MAX_DESC_LEN, "%s", description);
        push_back(parser.flags, def);
        return parser.flags.cap - 1;
    }

    U64 add_option(Parser& parser, const char* long_name, const char* short_name, const char* description, const char* default_value) {
        OptionDef def{};
        snprintf(def.long_name,     MAX_NAME_LEN,  "%s", long_name);
        snprintf(def.short_name,    8,             "%s", short_name);
        snprintf(def.description,   MAX_DESC_LEN,  "%s", description);
        snprintf(def.default_value, MAX_VALUE_LEN, "%s", default_value);
        push_back(parser.options, def);
        return parser.options.cap - 1;
    }

    ParseResult parse(const Parser& parser, int argc, char* argv[]) {
        ParseResult result{};
        result.ok = true;

        for (U64 oi = 0; oi < parser.options.cap; oi++)
            snprintf(result.option_values[oi], MAX_VALUE_LEN, "%s", parser.options[oi].default_value);

        for (int i = 1; i < argc; i++) {
            const char* arg = argv[i];

            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                result.help_requested = true;
                return result;
            }

            if (arg[0] == '-') {
                bool found = false;
                for (U64 oi = 0; oi < parser.options.cap; oi++) {
                    const OptionDef& opt = parser.options[oi];
                    if (strcmp(arg, opt.long_name) == 0 ||
                        (opt.short_name[0] != '\0' && strcmp(arg, opt.short_name) == 0))
                    {
                        if (i + 1 >= argc) {
                            snprintf(result.error, MAX_DESC_LEN, "%s requires a value", arg);
                            result.ok = false;
                            return result;
                        }
                        snprintf(result.option_values[oi], MAX_VALUE_LEN, "%s", argv[++i]);
                        found = true;
                        break;
                    }
                }
                if (found) continue;

                for (U64 fi = 0; fi < parser.flags.cap; fi++) {
                    const FlagDef& flag = parser.flags[fi];
                    if (strcmp(arg, flag.long_name) == 0 ||
                        (flag.short_name[0] != '\0' && strcmp(arg, flag.short_name) == 0))
                    {
                        result.flag_bits |= (U32(1) << fi);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(result.error, MAX_DESC_LEN, "unknown option: %s", arg);
                    result.ok = false;
                    return result;
                }
            } else {
                if (result.positional_count >= parser.positionals.cap) {
                    snprintf(result.error, MAX_DESC_LEN, "unexpected argument: %s", arg);
                    result.ok = false;
                    return result;
                }
                snprintf(result.positional_values[result.positional_count], MAX_VALUE_LEN, "%s", arg);
                result.positional_count++;
            }
        }

        for (U64 i = result.positional_count; i < parser.positionals.cap; i++) {
            if (!parser.positionals[i].optional) {
                snprintf(result.error, MAX_DESC_LEN, "missing required argument <%s>", parser.positionals[i].name);
                result.ok = false;
                break;
            }
        }

        return result;
    }

    void print_help(const Parser& parser) {
#if PLEXDB_OS_WINDOWS
        printf("Usage: %s", parser.prog_name);
#else
        printf("usage: %s", parser.prog_name);
#endif
        for (U64 i = 0; i < parser.positionals.cap; i++) {
            if (parser.positionals[i].optional)
                printf(" [<%s>]", parser.positionals[i].name);
            else
                printf(" <%s>", parser.positionals[i].name);
        }
        for (U64 i = 0; i < parser.options.cap; i++) {
            printf(" [--%s <%s>]", parser.options[i].long_name + 2, parser.options[i].long_name + 2);
        }
        for (U64 i = 0; i < parser.flags.cap; i++) {
            printf(" [%s]", parser.flags[i].long_name);
        }
        printf(" [-h]\n");

        if (parser.description[0] != '\0') {
            printf("\n%s\n", parser.description);
        }

        if (parser.positionals.cap > 0) {
            printf("\nArguments:\n");
            for (U64 i = 0; i < parser.positionals.cap; i++) {
                char display[MAX_NAME_LEN + 4];
                if (parser.positionals[i].optional)
                    snprintf(display, sizeof(display), "[<%s>]", parser.positionals[i].name);
                else
                    snprintf(display, sizeof(display), "<%s>", parser.positionals[i].name);
                printf("  %-20s  %s\n", display, parser.positionals[i].description);
            }
        }

        if (parser.options.cap > 0 || parser.flags.cap > 0) {
            printf("\nOptions:\n");
            for (U64 i = 0; i < parser.options.cap; i++) {
                const OptionDef& opt = parser.options[i];
                char name_val[MAX_NAME_LEN + 8];
                snprintf(name_val, sizeof(name_val), "%s <%s>", opt.long_name, opt.long_name + 2);
                if (opt.short_name[0] != '\0') {
                    printf("  %s, %-20s  %s (default: %s)\n", opt.short_name, name_val, opt.description, opt.default_value);
                } else {
                    printf("  %-24s  %s (default: %s)\n", name_val, opt.description, opt.default_value);
                }
            }
            for (U64 i = 0; i < parser.flags.cap; i++) {
                const FlagDef& flag = parser.flags[i];
                if (flag.short_name[0] != '\0') {
                    printf("  %s, %-20s  %s\n", flag.short_name, flag.long_name, flag.description);
                } else {
                    printf("  %-24s  %s\n", flag.long_name, flag.description);
                }
            }
        }

        printf("\n  -h, %-20s  Show this help message\n", "--help");
    }

    String8 get_positional(const ParseResult& result, U64 index) {
        assert_true(index < result.positional_count, "positional index out of range");
        return String8(result.positional_values[index]);
    }

    String8 get_optional_positional(const ParseResult& result, U64 index) {
        if (index >= result.positional_count) return String8{};
        return String8(result.positional_values[index]);
    }

    bool has_flag(const ParseResult& result, U64 flag_index) {
        assert_true(flag_index < MAX_FLAGS, "flag index out of range");
        return (result.flag_bits & (U32(1) << flag_index)) != 0;
    }

    String8 get_option(const ParseResult& result, U64 option_index) {
        assert_true(option_index < MAX_OPTIONS, "option index out of range");
        return String8(result.option_values[option_index]);
    }
}
