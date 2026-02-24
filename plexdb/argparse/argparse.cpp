module;
#include <string.h>
#include <stdio.h>
#include "macros.h"

module plexdb.argparse;

namespace plexdb::argparse {
    Parser make_parser(const char* prog_name, const char* description) {
        Parser p{};
        snprintf(p.prog_name,   MAX_NAME_LEN, "%s", prog_name);
        snprintf(p.description, MAX_DESC_LEN, "%s", description);
        return p;
    }

    void add_positional(Parser& parser, const char* name, const char* description) {
        PositionalDef def{};
        snprintf(def.name,        MAX_NAME_LEN, "%s", name);
        snprintf(def.description, MAX_DESC_LEN, "%s", description);
        push_back(parser.positionals, def);
    }

    void add_flag(Parser& parser, const char* long_name, const char* short_name, const char* description) {
        FlagDef def{};
        snprintf(def.long_name,   MAX_NAME_LEN, "%s", long_name);
        snprintf(def.short_name,  8,            "%s", short_name);
        snprintf(def.description, MAX_DESC_LEN, "%s", description);
        push_back(parser.flags, def);
    }

    ParseResult parse(const Parser& parser, int argc, char* argv[]) {
        ParseResult result{};
        result.ok = true;

        for (int i = 1; i < argc; i++) {
            const char* arg = argv[i];

            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                result.help_requested = true;
                return result;
            }

            if (arg[0] == '-') {
                bool found = false;
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

        if (result.positional_count < parser.positionals.cap) {
            const PositionalDef& missing = parser.positionals[result.positional_count];
            snprintf(result.error, MAX_DESC_LEN, "missing required argument <%s>", missing.name);
            result.ok = false;
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
            printf(" <%s>", parser.positionals[i].name);
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
                printf("  %-20s  %s\n", parser.positionals[i].name, parser.positionals[i].description);
            }
        }

        if (parser.flags.cap > 0) {
            printf("\nOptions:\n");
            for (U64 i = 0; i < parser.flags.cap; i++) {
                const FlagDef& flag = parser.flags[i];
                if (flag.short_name[0] != '\0') {
                    printf("  %s, %-16s  %s\n", flag.short_name, flag.long_name, flag.description);
                } else {
                    printf("  %-20s  %s\n", flag.long_name, flag.description);
                }
            }
        }

        printf("\n  -h, %-16s  Show this help message\n", "--help");
    }

    String8 get_positional(const ParseResult& result, U64 index) {
        assert_true(index < result.positional_count, "positional index out of range");
        return String8(result.positional_values[index]);
    }

    bool has_flag(const ParseResult& result, U64 flag_index) {
        assert_true(flag_index < MAX_FLAGS, "flag index out of range");
        return (result.flag_bits & (U32(1) << flag_index)) != 0;
    }
}
