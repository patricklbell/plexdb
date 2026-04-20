---
name: lexy-parsing
description: Write C++ parsers using the lexy parsing DSL. Use when defining new grammar rules, productions, callbacks, or error handlers with lexy. Covers the production struct pattern, dsl:: combinators, callbacks, and lexy::parse invocation as used in this codebase.
compatibility: C++20, header-only lexy library. Include lexy/dsl.hpp, lexy/callback.hpp, lexy/action/parse.hpp, lexy/input/string_input.hpp.
---

## Core concepts

A lexy grammar is a set of **productions**: structs with three static constexpr members:

```cpp
struct my_production {
    static constexpr auto rule  = /* dsl expression */;
    static constexpr auto whitespace = /* optional: dsl expression for auto-ws-skip */;
    static constexpr auto value = /* callback or sink */;
};
```

All DSL combinators live in `lexy::dsl`, typically aliased as `namespace dsl = lexy::dsl;`.

---

## Grammar rules

### Literals

```cpp
LEXY_LIT("keyword")                       // exact case-sensitive match
LEXY_LIT_CI("keyword")                    // case-insensitive (uses ascii::case_folding internally)
dsl::lit_c<';'>                           // single character
```

### Character classes

```cpp
dsl::ascii::alpha                         // [a-zA-Z]
dsl::ascii::alpha_underscore              // [a-zA-Z_]  — for identifier start
dsl::ascii::alpha_digit_underscore        // [a-zA-Z0-9_] — for identifier continuation
dsl::ascii::space / dsl::ascii::newline   // whitespace characters
dsl::digit<>                              // [0-9]
-dsl::ascii::control                      // any byte that is NOT a control char
```

### Identifiers

```cpp
struct identifier {
    static constexpr auto rule  = dsl::identifier(dsl::ascii::alpha_underscore,
                                                   dsl::ascii::alpha_digit_underscore);
    static constexpr auto value = lexy::as_string<String8>;
};
```

### Numbers

```cpp
struct integer_literal {
    static constexpr auto rule = []() {
        auto plus_minus = dsl::lit_c<'-'> / dsl::lit_c<'+'>;
        return dsl::peek(plus_minus / dsl::digit<>) >> dsl::sign + dsl::integer<S64>;
    }();
    static constexpr auto value = lexy::as_integer<S64>;
};
```

### Delimited strings (with escape sequences)

```cpp
struct string_literal {
    static constexpr auto escaped_symbols = lexy::symbol_table<char>
        .map<'\''>('\'')
        .map<'\\'>('\\')
        .map<'n'>('\n')
        .map<'t'>('\t');

    static constexpr auto rule = [] {
        auto escape = dsl::backslash_escape.symbol<escaped_symbols>();
        return dsl::delimited(LEXY_LIT("'"))(-dsl::ascii::control, escape);
    }();
    static constexpr auto value = lexy::as_string<std::string>; // or AutoString8
};
```

Multiple delimiters: `dsl::delimited(LEXY_LIT("'") | LEXY_LIT("$$"))(content, escape)`.

---

## Branch rules and sequencing

### Sequencing: `+`

```cpp
rule_a + rule_b              // parse a then b
```

### Ordered choice: `|`

```cpp
rule_a | rule_b              // try a, if it fails try b
```

**Order matters for branch choices** — put longer/more-specific matches first:

```cpp
// ne before eq, le before lt, ge before gt
dsl::p<ne> | dsl::p<le> | dsl::p<ge> | dsl::p<lt> | dsl::p<gt> | dsl::p<eq>
```

### Peek + commit: `dsl::peek(cond) >> body`

`peek` tests without consuming input. `>>` means "if the condition matches, commit and parse body".

```cpp
// Optional clause: only consume if keyword is present
static constexpr auto rule = []() {
    auto key = kw_if + dsl::p<ws> + kw_not + dsl::p<ws> + kw_exists;
    return dsl::opt(dsl::peek(key) >> key);
}();
```

### Optional: `dsl::opt`

```cpp
dsl::opt(some_rule)
// produces lexy::nullopt if not matched; callback receives it as the absent case
```

---

## Productions and calling them

### Call a production: `dsl::p<P>`

```cpp
dsl::p<identifier>           // parse the identifier production
```

### Transparent productions

Mark helper sub-productions with `lexy::transparent_production` when you want the parse tree to be flat (the sub-production's rule is inlined into the parent):

```cpp
struct eq : lexy::transparent_production {
    static constexpr auto rule  = dsl::lit_c<'='>;
    static constexpr auto value = lexy::constant(ComparisonOp::eq);
};
```

### Whitespace production

Declare a whitespace production and reference it in other productions via `dsl::p<ws>` for manual skipping, or set `static constexpr auto whitespace` for automatic skipping after every token.

```cpp
struct ws {
    static constexpr auto rule  = dsl::whitespace(dsl::ascii::space / dsl::ascii::newline);
    static constexpr auto value = lexy::noop;
};
```

In this codebase whitespace is skipped manually between tokens using `dsl::p<ws>`.

---

## Lists

```cpp
// Comma-separated list, trailing comma is an error
struct trailing_comma_error {
    static constexpr auto name = "unexpected trailing comma";
};

static constexpr auto rule = dsl::list(
    dsl::p<item>,
    dsl::sep(dsl::lit_c<','> >> dsl::p<ws>).trailing_error<trailing_comma_error>
);
static constexpr auto value = lexy::as_list<MyContainer>;
```

Keyword separator: `dsl::sep(kw_and >> dsl::p<ws>)`.

---

## Callbacks and sinks

### `lexy::noop` — discard all values

```cpp
static constexpr auto value = lexy::noop;
```

### `lexy::forward<T>` — pass the single parsed value through

```cpp
static constexpr auto value = lexy::forward<Type>;
```

### `lexy::constant(v)` — always produce a fixed value

```cpp
static constexpr auto value = lexy::constant(ComparisonOp::eq);
```

### `lexy::construct<T>` — construct T from parsed values

```cpp
static constexpr auto value = lexy::construct<MyType>;
```

### `lexy::as_string<T>` — collect characters into a string type

```cpp
static constexpr auto value = lexy::as_string<AutoString8>;
```

### `lexy::as_integer<T>` — convert parsed digits to an integer

```cpp
static constexpr auto value = lexy::as_integer<S64>;
```

### `lexy::as_list<Container>` and `lexy::as_collection<Container>` — collect items into a container (sink)

Both are a combined callback + sink. The distinction:
- `as_list` — for positional containers (sequential insert); calls `.push_back()` / `.emplace_back()`
- `as_collection` — for non-positional containers (e.g. maps, sets); calls `.insert()` / `.emplace()`

```cpp
static constexpr auto value = lexy::as_list<CappedArray<Item, MAX>>;       // sequential
static constexpr auto value = lexy::as_collection<DynamicMap<K, V>>;       // map/set
```

### `lexy::as_aggregate<T>` — fill struct fields set with `dsl::member`

Requires using `dsl::member<&T::field> = dsl::p<sub_production>` inside the rule:

```cpp
static constexpr auto rule = [] {
    return (dsl::member<&MyStruct::column> = dsl::p<identifier>) + dsl::p<ws> +
           (dsl::member<&MyStruct::value>  = dsl::p<term_value>);
}();
static constexpr auto value = lexy::as_aggregate<MyStruct>;
```

### `lexy::callback<ReturnType>(lambdas...)` — overloaded callback

Provide one lambda per possible set of parsed values. Use `lexy::nullopt` as a parameter type to handle the absent case from `dsl::opt`:

```cpp
static constexpr auto value = lexy::callback<bool>(
    [](lexy::nullopt) { return false; },
    []()              { return true;  }
);
```

Multi-value callback:

```cpp
static constexpr auto value = lexy::callback<Pair<S64, S64>>(
    [](S64 ts, S64 ttl) { return Pair<S64, S64>{ts, ttl}; }
);
```

---

## Parsing (invoking the action)

```cpp
#include <lexy/action/parse.hpp>
#include <lexy/input/string_input.hpp>

// Create input from a string/buffer
auto input = lexy::string_input<lexy::ascii_encoding>(data, length);

// Parse and get result
auto result = lexy::parse<grammar::root_production>(input, MyErrorCallback{});

if (result.has_value()) {
    MyType value = result.value();
}
```

---

## Patterns used in this codebase

### Case-insensitive keyword macro

```cpp
#define LEXY_LIT_CI(x) lexy::dsl::ascii::case_folding(LEXY_LIT(x))
```

Declare keywords as `constexpr auto` at namespace scope:

```cpp
constexpr auto kw_select = LEXY_LIT_CI("select");
```

### Top-level statement dispatch

Combine statement productions with `|`:

```cpp
struct cql_statement {
    static constexpr auto rule = [] {
        return dsl::p<ws> + (dsl::p<select_stmt> | dsl::p<insert_stmt> | dsl::p<update_stmt>)
               + dsl::p<ws> + (dsl::lit_c<';'> | dsl::eof);
    }();
    static constexpr auto value = lexy::construct<Statement>;
};
```

### Scanning without parsing values

```cpp
struct query_complete_scanner {
    static constexpr auto rule  = dsl::until(dsl::lit_c<';'>);
    static constexpr auto value = lexy::noop;
};
```

### Enum variant via transparent sub-productions

```cpp
struct my_enum_production {
    struct variant_a : lexy::transparent_production {
        static constexpr auto rule  = LEXY_LIT_CI("asc");
        static constexpr auto value = lexy::constant(MyEnum::A);
    };
    struct variant_b : lexy::transparent_production {
        static constexpr auto rule  = LEXY_LIT_CI("desc");
        static constexpr auto value = lexy::constant(MyEnum::B);
    };
    static constexpr auto rule  = dsl::p<variant_a> | dsl::p<variant_b>;
    static constexpr auto value = lexy::forward<MyEnum>;
};
```

---
## More information
https://lexy.foonathan.net/reference/