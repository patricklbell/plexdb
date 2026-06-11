module;
#include <string.h>
#include <plexdb/macros/macros.h>

module plexdb.argparse;

namespace plexdb::argparse {
    namespace {
        void str_copy(char* dst, U64 dst_size, const char* src) {
            String8 view{dst, dst_size - 1};
            fmt_raw(view, "%s", src);
            dst[view.length] = '\0';
        }
    }

    Parser create_parser(const char* prog_name, const char* description) {
        Parser p{};
        str_copy(p.prog_name,   MAX_NAME_LEN, prog_name);
        str_copy(p.description, MAX_DESC_LEN, description);
        return p;
    }

    U64 add_positional(Parser& parser, const char* name, const char* description) {
        PositionalDef def{};
        str_copy(def.name,        MAX_NAME_LEN, name);
        str_copy(def.description, MAX_DESC_LEN, description);
        def.optional = false;
        push_back(parser.positionals, def);
        return parser.positionals.cap - 1;
    }

    U64 add_optional_positional(Parser& parser, const char* name, const char* description) {
        PositionalDef def{};
        str_copy(def.name,        MAX_NAME_LEN, name);
        str_copy(def.description, MAX_DESC_LEN, description);
        def.optional = true;
        push_back(parser.positionals, def);
        return parser.positionals.cap - 1;
    }

    U64 add_flag(Parser& parser, const char* long_name, const char* short_name, const char* description) {
        FlagDef def{};
        str_copy(def.long_name,   MAX_NAME_LEN, long_name);
        str_copy(def.short_name,  8,            short_name);
        str_copy(def.description, MAX_DESC_LEN, description);
        push_back(parser.flags, def);
        return parser.flags.cap - 1;
    }

    U64 add_option(Parser& parser, const char* long_name, const char* short_name, const char* description, const char* default_value) {
        OptionDef def{};
        str_copy(def.long_name,     MAX_NAME_LEN,  long_name);
        str_copy(def.short_name,    8,             short_name);
        str_copy(def.description,   MAX_DESC_LEN,  description);
        str_copy(def.default_value, MAX_VALUE_LEN, default_value);
        push_back(parser.options, def);
        return parser.options.cap - 1;
    }

    ParseResult parse(const Parser& parser, int argc, char* argv[]) {
        ParseResult result{};
        result.ok = true;

        for (U64 oi = 0; oi < parser.options.cap; oi++)
            str_copy(result.option_values[oi], MAX_VALUE_LEN, parser.options[oi].default_value);

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
                            String8 view{result.error, MAX_DESC_LEN - 1};
                            fmt_raw(view, "%s requires a value", arg);
                            result.error[view.length] = '\0';
                            result.ok = false;
                            return result;
                        }
                        str_copy(result.option_values[oi], MAX_VALUE_LEN, argv[++i]);
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
                    String8 view{result.error, MAX_DESC_LEN - 1};
                    fmt_raw(view, "unknown option: %s", arg);
                    result.error[view.length] = '\0';
                    result.ok = false;
                    return result;
                }
            } else {
                if (result.positional_count >= parser.positionals.cap) {
                    String8 view{result.error, MAX_DESC_LEN - 1};
                    fmt_raw(view, "unexpected argument: %s", arg);
                    result.error[view.length] = '\0';
                    result.ok = false;
                    return result;
                }
                str_copy(result.positional_values[result.positional_count], MAX_VALUE_LEN, arg);
                result.positional_count++;
            }
        }

        for (U64 i = result.positional_count; i < parser.positionals.cap; i++) {
            if (!parser.positionals[i].optional) {
                String8 view{result.error, MAX_DESC_LEN - 1};
                fmt_raw(view, "missing required argument <%s>", parser.positionals[i].name);
                result.error[view.length] = '\0';
                result.ok = false;
                break;
            }
        }

        return result;
    }

    void print_help(const Parser& parser) {
#if PLEXDB_OS_WINDOWS
        print(fmt("Usage: %s", parser.prog_name));
#else
        print(fmt("usage: %s", parser.prog_name));
#endif
        for (U64 i = 0; i < parser.positionals.cap; i++) {
            if (parser.positionals[i].optional)
                print(fmt(" [<%s>]", parser.positionals[i].name));
            else
                print(fmt(" <%s>", parser.positionals[i].name));
        }
        for (U64 i = 0; i < parser.options.cap; i++) {
            print(fmt(" [--%s <%s>]", parser.options[i].long_name + 2, parser.options[i].long_name + 2));
        }
        for (U64 i = 0; i < parser.flags.cap; i++) {
            print(fmt(" [%s]", parser.flags[i].long_name));
        }
        print(" [-h]\n");

        if (parser.description[0] != '\0') {
            print(fmt("\n%s\n", parser.description));
        }

        if (parser.positionals.cap > 0) {
            print("\nArguments:\n");
            for (U64 i = 0; i < parser.positionals.cap; i++) {
                auto display = parser.positionals[i].optional
                    ? fmt("[<%s>]", parser.positionals[i].name)
                    : fmt("<%s>",   parser.positionals[i].name);
                print(fmt("  %-20s  %s\n", display.c_str, parser.positionals[i].description));
            }
        }

        if (parser.options.cap > 0 || parser.flags.cap > 0) {
            print("\nOptions:\n");
            for (U64 i = 0; i < parser.options.cap; i++) {
                const OptionDef& opt = parser.options[i];
                auto name_val = fmt("%s <%s>", opt.long_name, opt.long_name + 2);
                if (opt.short_name[0] != '\0') {
                    print(fmt("  %s, %-20s  %s (default: %s)\n", opt.short_name, name_val.c_str, opt.description, opt.default_value));
                } else {
                    print(fmt("  %-24s  %s (default: %s)\n", name_val.c_str, opt.description, opt.default_value));
                }
            }
            for (U64 i = 0; i < parser.flags.cap; i++) {
                const FlagDef& flag = parser.flags[i];
                if (flag.short_name[0] != '\0') {
                    print(fmt("  %s, %-20s  %s\n", flag.short_name, flag.long_name, flag.description));
                } else {
                    print(fmt("  %-24s  %s\n", flag.long_name, flag.description));
                }
            }
        }

        print(fmt("\n  -h, %-20s  Show this help message\n", "--help"));
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
