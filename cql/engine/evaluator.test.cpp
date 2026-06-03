#include <catch2/catch_test_macros.hpp>
#include <plexdb/test_macros/test_macros.h>

import plexdb.base;
import plexdb.coroutine;
import plexdb.dynamic.containers;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import cql.engine.column_value;
import cql.engine.evaluator;
import cql.engine.io;
import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;
using namespace cql;
using namespace cql::io;

namespace {
    struct Buffer {
        DynamicArray<U8> data{};
        U64 cursor = 0;

        struct WriterCallable {
            Buffer* self;
            void operator()(const U8* src, U64 size) {
                for (U64 i = 0; i < size; i++) push_back(self->data, src[i]);
            }
        } _wc{nullptr};

        struct ReaderCallable {
            Buffer* self;
            coroutine::Task<void> operator()(U8* dst, U64 size) {
                for (U64 i = 0; i < size; i++) dst[i] = self->data[self->cursor + i];
                self->cursor += size;
                co_return;
            }
        } _rc{nullptr};

        Buffer() : _wc{this}, _rc{this} {}

        Writer writer() { return to_writer(_wc); }
        Reader reader() { return to_reader(_rc); }
    };

    Term constant_s64(S64 value) {
        return Term{Constant{value}};
    }

    Term constant_f64(F64 value) {
        return Term{Constant{value}};
    }

    Term constant_text(const char* value) {
        return Term{Constant{AutoString8(value)}};
    }

    U8 uuid_version(const UUID& uuid) {
        return static_cast<U8>(uuid.value[6] >> 4);
    }

    U8 uuid_variant(const UUID& uuid) {
        return static_cast<U8>(uuid.value[8] >> 6);
    }

    const Constant& as_constant(const Evaluated& evaluated) {
        REQUIRE(type_matches_tag<Constant>(evaluated.value));
        return get<Constant>(evaluated.value);
    }
}

TEST_CASE("evaluator - arithmetic", "[cql.engine.evaluator]") {
    EvalContext ctx{};

    SECTION("integer addition") {
        Term term{ArithmeticOperation{BinaryArithmeticOperation{constant_s64(1), ArithmeticOperator::plus, constant_s64(2)}}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(type_matches_tag<S64>(as_constant(out).value));
        REQUIRE(get<S64>(as_constant(out).value) == 3);
    }

    SECTION("mixed int and float promotes to float") {
        Term term{ArithmeticOperation{BinaryArithmeticOperation{constant_s64(2), ArithmeticOperator::times, constant_f64(1.5)}}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(type_matches_tag<F64>(as_constant(out).value));
        REQUIRE(get<F64>(as_constant(out).value) == 3.0);
    }

    SECTION("integer division truncates") {
        Term term{ArithmeticOperation{BinaryArithmeticOperation{constant_s64(5), ArithmeticOperator::divide, constant_s64(2)}}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == 2);
    }

    SECTION("modulus works for integers") {
        Term term{ArithmeticOperation{BinaryArithmeticOperation{constant_s64(17), ArithmeticOperator::mod, constant_s64(5)}}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == 2);
    }

    SECTION("string concatenation") {
        Term term{ArithmeticOperation{BinaryArithmeticOperation{constant_text("hel"), ArithmeticOperator::plus, constant_text("lo")}}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(type_matches_tag<AutoString8>(as_constant(out).value));
        REQUIRE(get<AutoString8>(as_constant(out).value) == "hello");
    }

    SECTION("unary minus") {
        Term term{ArithmeticOperation{UnaryMinusArithmeticOperation{constant_s64(7)}}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == -7);
    }
}

TEST_CASE("evaluator - bind markers and type hints", "[cql.engine.evaluator]") {
    DynamicArray<Constant> positional{};
    push_back(positional, Constant{S64(41)});

    DynamicArray<Pair<AutoString8, Constant>> named{};
    push_back(named, Pair<AutoString8, Constant>{AutoString8("answer"), Constant{S64(42)}});

    EvalContext ctx{};
    ctx.positional_bindings = positional;
    ctx.named_bindings = named;

    SECTION("positional bind marker uses the first binding") {
        Term term{BindMarker{AutoString8("0")}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == 41);
    }

    SECTION("named bind marker resolves by name") {
        Term term{BindMarker{AutoString8("answer")}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == 42);
    }

    SECTION("type hint preserves the evaluated value") {
        Term term{TypeHint{type::create_basic(type::Basic::int_), constant_s64(123)}};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == 123);
    }
}

TEST_CASE("evaluator - builtins", "[cql.engine.evaluator]") {
    EvalContext ctx{};

    SECTION("uuid() returns a version 4 uuid") {
        FunctionCall call;
        call.identifier = AutoString8("uuid");
        Term term{move(call)};

        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(type_matches_tag<UUID>(as_constant(out).value));
        REQUIRE(uuid_version(get<UUID>(as_constant(out).value)) == 4);
        REQUIRE(uuid_variant(get<UUID>(as_constant(out).value)) == 2);
    }

    SECTION("now() returns a version 1 timeuuid") {
        FunctionCall call;
        call.identifier = AutoString8("now");
        Term term{move(call)};

        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(type_matches_tag<UUID>(as_constant(out).value));
        REQUIRE(uuid_version(get<UUID>(as_constant(out).value)) == 1);
        REQUIRE(uuid_variant(get<UUID>(as_constant(out).value)) == 2);
    }

    SECTION("toTimestamp(minTimeuuid(x)) round-trips the timestamp") {
        FunctionCall inner_call;
        inner_call.identifier = AutoString8("minTimeuuid");
        push_back(inner_call.arguments, constant_s64(1234567890));

        FunctionCall outer_call;
        outer_call.identifier = AutoString8("toTimestamp");
        push_back(outer_call.arguments, Term{move(inner_call)});

        Term term{move(outer_call)};
        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(type_matches_tag<S64>(as_constant(out).value));
        REQUIRE(get<S64>(as_constant(out).value) == 1234567890);
    }

    SECTION("toDate(timestamp) returns days since epoch") {
        FunctionCall call;
        call.identifier = AutoString8("toDate");
        push_back(call.arguments, constant_s64(86400000));
        Term term{move(call)};

        Evaluated out = evaluate(term, ctx);
        REQUIRE(type_matches_tag<Constant>(out.value));
        REQUIRE(get<S64>(as_constant(out).value) == 1);
    }
}

IO_TEST_CASE("evaluator - nested expression evaluation in collection writes", "[cql.engine.evaluator]") {
    DynamicArray<Constant> positional{};
    push_back(positional, Constant{S64(4)});

    EvalContext ctx{};
    ctx.positional_bindings = positional;

    ListOrVectorLiteral list{};
    push_back(list.elements, Term{ArithmeticOperation{BinaryArithmeticOperation{constant_s64(1), ArithmeticOperator::plus, constant_s64(2)}}});
    push_back(list.elements, Term{BindMarker{AutoString8("0")}});

    FunctionCall inner_call;
    inner_call.identifier = AutoString8("minTimeuuid");
    push_back(inner_call.arguments, constant_s64(0));

    FunctionCall outer_call;
    outer_call.identifier = AutoString8("toTimestamp");
    push_back(outer_call.arguments, Term{move(inner_call)});
    push_back(list.elements, Term{move(outer_call)});

    Term term{move(list)};
    Evaluated evaluated = evaluate(term, ctx);

    Buffer buf;
    cast_write_evaluated_as_column_value(buf.writer(), evaluated, type::create_list(type::Basic::bigint), ctx);

    auto out = co_await read_column_value(buf.reader(), type::create_list(type::Basic::bigint));
    REQUIRE(type_matches_tag<DynamicArray<NestedColumnValue>>(out));
    auto& arr = get<DynamicArray<NestedColumnValue>>(out);
    REQUIRE(arr.length == 3);
    REQUIRE(get<S64>(arr[0].value) == 3);
    REQUIRE(get<S64>(arr[1].value) == 4);
    REQUIRE(get<S64>(arr[2].value) == 0);
}
