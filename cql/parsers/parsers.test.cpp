#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;
import plexdb.dynamic.containers;

import cql.parsers;
import cql.engine.statements;
import cql.engine.types;

using namespace plexdb;
using namespace cql;
using namespace cql::parsers;

namespace {
    std::string g_parse_errors;

    void collect_parse_error(const String8& error) {
        g_parse_errors.append(error.data, error.length);
        g_parse_errors.push_back('\n');
    }
}

TEST_CASE("CQL CREATE KEYSPACE", "[cql.cql]") {
    SECTION("basic") {
        auto result = parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': '1'};");
        REQUIRE(result.has_value());
        auto& stmt = get<CreateKeyspace>(result->value);
        REQUIRE(stmt.name == "ks");
        REQUIRE(stmt.if_not_exists == false);
    }
    SECTION("if not exists") {
        auto result = parse("CREATE KEYSPACE IF NOT EXISTS ks WITH replication = {'class': 'SimpleStrategy'};");
        REQUIRE(result.has_value());
        auto& stmt = get<CreateKeyspace>(result->value);
        REQUIRE(stmt.if_not_exists == true);
    }
}

TEST_CASE("CQL USE KEYSPACE", "[cql.cql]") {
    auto result = parse("USE my_keyspace;");
    REQUIRE(result.has_value());
    auto& stmt = get<UseKeyspace>(result->value);
    REQUIRE(stmt.keyspace == "my_keyspace");
}

TEST_CASE("CQL DROP KEYSPACE", "[cql.cql]") {
    auto result = parse("DROP KEYSPACE IF EXISTS ks;");
    REQUIRE(result.has_value());
    auto& stmt = get<DropKeyspace>(result->value);
    REQUIRE(stmt.if_exists == true);
    REQUIRE(stmt.keyspace == "ks");
}

TEST_CASE("CQL CREATE KEYSPACE statements", "[cql.parser]") {
    SECTION("Basic CREATE KEYSPACE") {
        auto query  = "CREATE KEYSPACE my_keyspace WITH replication = 'SimpleStrategy';";
        auto result = parse(query);
    }
}

TEST_CASE("CQL CREATE TABLE", "[cql.cql]") {
    SECTION("simple table with inline primary key") {
        auto result = parse("CREATE TABLE ks.tbl (id int PRIMARY KEY, name text, age int);");

        REQUIRE(result.has_value());
        auto& stmt = get<CreateTable>(result->value);
        REQUIRE(stmt.name.table_name == "tbl");
        REQUIRE(stmt.column_definitions.length == 3);
    }

    SECTION("CREATE KEYSPACE IF NOT EXISTS") {
        auto query  = "CREATE KEYSPACE IF NOT EXISTS test_ks WITH replication = 'NetworkTopologyStrategy';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));

        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.name == "test_ks");
        REQUIRE(ks.if_not_exists == true);
        REQUIRE(ks.options.identifier_values.length == 1);
        REQUIRE(ks.options.identifier_values[0].first == "replication");
        // @note quoted option values are stored as Constant{AutoString8}
        REQUIRE(type_matches_tag<Constant>(ks.options.identifier_values[0].second));
        REQUIRE(get<AutoString8>(get<Constant>(ks.options.identifier_values[0].second).value) == "NetworkTopologyStrategy");
    }

    SECTION("CREATE KEYSPACE with multiple options") {
        auto query  = "CREATE KEYSPACE prod WITH replication = 'SimpleStrategy' AND durable_writes = 'true';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));

        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.name == "prod");
        REQUIRE(ks.if_not_exists == false);
        REQUIRE(ks.options.identifier_values.length == 2);
        REQUIRE(ks.options.identifier_values[0].first == "replication");
        // @note quoted option values are stored as Constant{AutoString8}
        REQUIRE(type_matches_tag<Constant>(ks.options.identifier_values[0].second));
        REQUIRE(get<AutoString8>(get<Constant>(ks.options.identifier_values[0].second).value) == "SimpleStrategy");
        REQUIRE(ks.options.identifier_values[1].first == "durable_writes");
        REQUIRE(type_matches_tag<Constant>(ks.options.identifier_values[1].second));
        REQUIRE(get<AutoString8>(get<Constant>(ks.options.identifier_values[1].second).value) == "true");
    }

    SECTION("CREATE KEYSPACE with three options") {
        auto query  = "CREATE KEYSPACE multi_opt WITH replication = 'NetworkTopologyStrategy' AND durable_writes = 'true' AND strategy_class = 'SimpleStrategy';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));

        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 3);
        REQUIRE(ks.options.identifier_values[0].first == "replication");
        REQUIRE(ks.options.identifier_values[1].first == "durable_writes");
        REQUIRE(ks.options.identifier_values[2].first == "strategy_class");
    }

    SECTION("CREATE KEYSPACE case insensitive") {
        auto query  = "create keyspace TestKS with replication = 'test';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));

        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.name == "testks"); // @note CQL folds unquoted identifiers to lowercase
    }

    SECTION("CREATE KEYSPACE with underscore in name") {
        auto query  = "CREATE KEYSPACE my_test_keyspace WITH replication = 'SimpleStrategy';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.name == "my_test_keyspace");
    }

    SECTION("CREATE KEYSPACE with mixed case IF NOT EXISTS") {
        auto query  = "CREATE KEYSPACE If Not Exists mixed_case WITH replication = 'test';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.if_not_exists == true);
        REQUIRE(ks.name == "mixed_case");
    }

    SECTION("CREATE KEYSPACE with quoted option value") {
        // @note CQL uses '' to escape single quotes inside strings, not backslash
        auto query  = "CREATE KEYSPACE ks WITH replication = '{''class'': ''SimpleStrategy''}';";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 1);
    }
}

TEST_CASE("CQL CREATE TABLE statements", "[cql.parser]") {
    SECTION("Basic CREATE TABLE with single column") {
        auto query  = "CREATE TABLE ks.users (id int PRIMARY KEY);";
        auto result = parse(query);
    }

    SECTION("if not exists") {
        auto result = parse("CREATE TABLE IF NOT EXISTS tbl (id int PRIMARY KEY);");
        REQUIRE(result.has_value());
        auto& stmt = get<CreateTable>(result->value);
        REQUIRE(stmt.if_not_exists == true);
    }
}

TEST_CASE("CQL DROP TABLE", "[cql.cql]") {
    auto result = parse("DROP TABLE IF EXISTS ks.tbl;");
    REQUIRE(result.has_value());
    auto& stmt = get<DropTable>(result->value);
    REQUIRE(stmt.if_exists == true);
    REQUIRE(stmt.table.table_name == "tbl");
}

TEST_CASE("CQL TRUNCATE", "[cql.cql]") {
    auto result = parse("TRUNCATE TABLE ks.tbl;");
    REQUIRE(result.has_value());
    auto& stmt = get<TruncateTable>(result->value);
    REQUIRE(stmt.table.table_name == "tbl");
}

TEST_CASE("CQL INSERT INTO", "[cql.cql]") {
    SECTION("with column names and values") {
        auto result = parse("INSERT INTO tbl (id, name) VALUES (1, 'hello');");
        REQUIRE(result.has_value());
        auto& stmt = get<Insert>(result->value);
        REQUIRE(stmt.table.table_name == "tbl");
        auto& nv = get<Insert::NamesValues>(stmt.insert_clause);
        REQUIRE(nv.names.length == 2);
        REQUIRE(nv.values.length == 2);
    }

    SECTION("CREATE TABLE with multiple columns") {
        auto query  = "CREATE TABLE ks.users (id int PRIMARY KEY, name text, age int);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTable>(result->value));

        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.name.table_name == "users");
        REQUIRE(tbl.column_definitions.length == 3);

        REQUIRE(tbl.column_definitions[0].name.identifier == "id");
        REQUIRE(tbl.column_definitions[0].type == type::create_basic(type::Basic::int_));
        REQUIRE(tbl.column_definitions[0].primary_key == true);

        REQUIRE(tbl.column_definitions[1].name.identifier == "name");
        REQUIRE(tbl.column_definitions[1].type == type::create_basic(type::Basic::text));
        REQUIRE(tbl.column_definitions[1].primary_key == false);

        REQUIRE(tbl.column_definitions[2].name.identifier == "age");
        REQUIRE(tbl.column_definitions[2].type == type::create_basic(type::Basic::int_));
        REQUIRE(tbl.column_definitions[2].primary_key == false);
    }

    SECTION("CREATE TABLE IF NOT EXISTS") {
        auto query  = "CREATE TABLE IF NOT EXISTS ks.products (sku int PRIMARY KEY, name text, price int);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTable>(result->value));

        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.name.table_name == "products");
        REQUIRE(tbl.if_not_exists == true);
        REQUIRE(tbl.column_definitions.length == 3);
    }

    SECTION("CREATE TABLE with various data types") {
        auto query  = "CREATE TABLE ks.data (id int PRIMARY KEY, name text, count bigint, created timestamp, active boolean);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 5);
        REQUIRE(tbl.column_definitions[0].type == type::create_basic(type::Basic::int_));
        REQUIRE(tbl.column_definitions[1].type == type::create_basic(type::Basic::text));
        REQUIRE(tbl.column_definitions[2].type == type::create_basic(type::Basic::bigint));
        REQUIRE(tbl.column_definitions[3].type == type::create_basic(type::Basic::timestamp));
        REQUIRE(tbl.column_definitions[4].type == type::create_basic(type::Basic::boolean));
    }

    SECTION("CREATE TABLE with FLOAT and DOUBLE types") {
        auto query  = "CREATE TABLE prod.metrics (id int PRIMARY KEY, temperature float, precision_value double);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 3);
        REQUIRE(tbl.column_definitions[1].type == type::create_basic(type::Basic::float_));
        REQUIRE(tbl.column_definitions[2].type == type::create_basic(type::Basic::double_));
    }

    SECTION("CREATE TABLE with UUID type") {
        auto query  = "CREATE TABLE ks.sessions (session_id uuid PRIMARY KEY, user_id int);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 2);
        REQUIRE(tbl.column_definitions[0].type == type::create_basic(type::Basic::uuid));
        REQUIRE(tbl.column_definitions[0].primary_key == true);
    }

    SECTION("CREATE TABLE case insensitive") {
        auto query  = "create table ks.TestTable (Id INT primary key, Name TEXT);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTable>(result->value));

        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.name.table_name == "testtable"); // @note case folding
    }

    SECTION("CREATE TABLE with non-primary key as last column") {
        auto query  = "CREATE TABLE ks.test (id int PRIMARY KEY, data text);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions[0].primary_key == true);
        REQUIRE(tbl.column_definitions[1].primary_key == false);
    }

    SECTION("CREATE TABLE with primary key in middle") {
        auto query  = "CREATE TABLE ks.test (name text, id int PRIMARY KEY, email text);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 3);
        REQUIRE(tbl.column_definitions[0].primary_key == false);
        REQUIRE(tbl.column_definitions[1].primary_key == true);
        REQUIRE(tbl.column_definitions[2].primary_key == false);
    }

    SECTION("CREATE TABLE with many columns") {
        auto query  = "CREATE TABLE ks.large (c1 int PRIMARY KEY, c2 text, c3 bigint, c4 timestamp, c5 boolean, c6 float, c7 double);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 7);
    }

    SECTION("CREATE TABLE with underscore column names") {
        auto query  = "CREATE TABLE ks.test (user_id int PRIMARY KEY, first_name text, last_name text);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions[0].name.identifier == "user_id");
        REQUIRE(tbl.column_definitions[1].name.identifier == "first_name");
        REQUIRE(tbl.column_definitions[2].name.identifier == "last_name");
    }
}

TEST_CASE("CQL INSERT INTO statements", "[cql.parser]") {
    SECTION("INSERT INTO with integer values") {
        auto query  = "INSERT INTO ks.users VALUES (1, 2, 3);";
        auto result = parse(query);
    }

    SECTION("if not exists") {
        auto result = parse("INSERT INTO tbl (id) VALUES (1) IF NOT EXISTS;");
        REQUIRE(result.has_value());
        auto& stmt = get<Insert>(result->value);
        REQUIRE(stmt.if_not_exists == true);
    }
}

TEST_CASE("CQL SELECT", "[cql.cql]") {
    SECTION("select star") {
        auto result = parse("SELECT * FROM tbl;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.from.table_name == "tbl");
    }

    SECTION("INSERT INTO with string values") {
        auto query  = "INSERT INTO my_ks.table VALUES ('text1', 'text2');";
        auto result = parse(query);
    }

    SECTION("select columns") {
        auto result = parse("SELECT id, name FROM ks.tbl;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.from.table_name == "tbl");
    }

    SECTION("INSERT INTO with mixed values") {
        auto query  = "INSERT INTO app.users VALUES (123, 'John Doe', 'john@example.com');";
        auto result = parse(query);
    }

    SECTION("select with where") {
        auto result = parse("SELECT * FROM tbl WHERE id = 1;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.where.has_value());
    }

    SECTION("INSERT INTO with single value") {
        auto query  = "INSERT INTO test.data VALUES (42);";
        auto result = parse(query);
    }

    SECTION("select with limit") {
        auto result = parse("SELECT * FROM tbl LIMIT 10;");
        REQUIRE(result.has_value());
        [[maybe_unused]] auto& stmt = get<Select>(result->value);
    }

    SECTION("INSERT INTO with negative integers") {
        // @note negation is represented as UnaryMinusArithmeticOperation, not a folded Constant
        auto query  = "INSERT INTO ks.data VALUES (-100, -50, -1);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        const auto& nv  = get<Insert::NamesValues>(ins.insert_clause);
        REQUIRE(nv.values.length == 3);
        auto check_neg = [](const Term& t, S64 expected) {
            const auto& arith = get<ArithmeticOperation>(t.value);
            const auto& unary = get<UnaryMinusArithmeticOperation>(arith.value);
            REQUIRE(get<S64>(get<Constant>(unary.operand.value).value) == expected);
        };
        check_neg(nv.values[0], 100);
        check_neg(nv.values[1], 50);
        check_neg(nv.values[2], 1);
    }

    SECTION("INSERT INTO arithmetic precedence") {
        auto query = "INSERT INTO ks.data VALUES (1 + 2 * 3 - 5);";
        g_parse_errors.clear();
        auto result = parse(query, &collect_parse_error);

        UNSCOPED_INFO(g_parse_errors);
        REQUIRE(result.has_value());
        const auto& ins  = get<Insert>(result->value);
        const auto& expr = get<ArithmeticOperation>(get<Insert::NamesValues>(ins.insert_clause).values[0].value);
        const auto& top  = get<BinaryArithmeticOperation>(expr.value);
        REQUIRE(top.op == ArithmeticOperator::minus);

        const auto& left     = get<ArithmeticOperation>(top.lhs.value);
        const auto& left_bin = get<BinaryArithmeticOperation>(left.value);
        REQUIRE(left_bin.op == ArithmeticOperator::plus);
        REQUIRE(get<S64>(get<Constant>(left_bin.lhs.value).value) == 1);

        const auto& times_expr = get<ArithmeticOperation>(left_bin.rhs.value);
        const auto& times_bin  = get<BinaryArithmeticOperation>(times_expr.value);
        REQUIRE(times_bin.op == ArithmeticOperator::times);
        REQUIRE(get<S64>(get<Constant>(times_bin.lhs.value).value) == 2);
        REQUIRE(get<S64>(get<Constant>(times_bin.rhs.value).value) == 3);
        REQUIRE(get<S64>(get<Constant>(top.rhs.value).value) == 5);
    }

    SECTION("INSERT INTO modulo operator") {
        auto query = "INSERT INTO ks.data VALUES (20 % 6);";
        g_parse_errors.clear();
        auto result = parse(query, &collect_parse_error);

        UNSCOPED_INFO(g_parse_errors);
        REQUIRE(result.has_value());
        const auto& ins     = get<Insert>(result->value);
        const auto& expr    = get<ArithmeticOperation>(get<Insert::NamesValues>(ins.insert_clause).values[0].value);
        const auto& mod_bin = get<BinaryArithmeticOperation>(expr.value);
        REQUIRE(mod_bin.op == ArithmeticOperator::mod);
        REQUIRE(get<S64>(get<Constant>(mod_bin.lhs.value).value) == 20);
        REQUIRE(get<S64>(get<Constant>(mod_bin.rhs.value).value) == 6);
    }

    SECTION("INSERT INTO named values with arithmetic expression") {
        auto query = "INSERT INTO ks.data (id, tag) VALUES (1 + 2 * 3, 'mul_precedence');";
        g_parse_errors.clear();
        auto result = parse(query, &collect_parse_error);
        UNSCOPED_INFO(g_parse_errors);
        REQUIRE(result.has_value());
    }

    SECTION("INSERT INTO named values with string concatenation") {
        auto query = "INSERT INTO ks.data (id, tag) VALUES (1, 'he' + 'llo');";
        g_parse_errors.clear();
        auto result = parse(query, &collect_parse_error);
        UNSCOPED_INFO(g_parse_errors);
        REQUIRE(result.has_value());
    }

    SECTION("INSERT INTO named values with function call") {
        auto query = "INSERT INTO ks.data (id, tag) VALUES (toDate(86400000), 'date');";
        g_parse_errors.clear();
        auto result = parse(query, &collect_parse_error);
        UNSCOPED_INFO(g_parse_errors);
        REQUIRE(result.has_value());
    }

    SECTION("INSERT INTO with large integer") {
        auto query  = "INSERT INTO ks.data VALUES (9223372036854775807);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 1);
        REQUIRE(get<S64>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == 9223372036854775807LL);
    }

    SECTION("INSERT INTO case insensitive") {
        auto query  = "insert into ks.tbl values (1, 'test');";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
    }

    SECTION("INSERT INTO with empty string") {
        auto query  = "INSERT INTO ks.tbl VALUES ('');";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 1);
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == "");
    }

    SECTION("INSERT INTO with string containing spaces") {
        auto query  = "INSERT INTO ks.tbl VALUES ('hello world', 'foo bar baz');";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 2);
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == "hello world");
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[1].value).value) == "foo bar baz");
    }

    SECTION("INSERT INTO with escaped quotes") {
        // @note CQL uses '' to escape single quotes inside strings, not backslash
        auto query  = "INSERT INTO ks.tbl VALUES ('''quoted''');";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 1);
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == "'quoted'");
    }

    SECTION("INSERT INTO with zero value") {
        auto query  = "INSERT INTO ks.tbl VALUES (0);";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<S64>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == 0);
    }

    SECTION("INSERT INTO with multiple string values") {
        auto query  = "INSERT INTO app.messages VALUES ('msg1', 'msg2', 'msg3', 'msg4', 'msg5');";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 5);
        for (size_t i = 0; i < 5; ++i) {
            REQUIRE(type_matches_tag<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value));
        }
    }
}

TEST_CASE("CQL SELECT FROM statements", "[cql.parser]") {
    SECTION("Basic SELECT FROM") {
        auto query  = "SELECT * FROM ks.users;";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));

        const auto& sel = get<Select>(result->value);
        REQUIRE(*sel.from.keyspace_name == "ks");
        REQUIRE(sel.from.table_name == "users");
    }

    SECTION("SELECT FROM case insensitive") {
        auto query  = "select * from TestTable;";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));

        const auto& sel = get<Select>(result->value);
        REQUIRE(!sel.from.keyspace_name);
        REQUIRE(sel.from.table_name == "testtable"); // @note CQL folds unquoted identifiers to lowercase
    }

    SECTION("SELECT FROM with underscores") {
        auto query  = "SELECT * FROM my_table;";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& sel = get<Select>(result->value);
        REQUIRE(!sel.from.keyspace_name);
        REQUIRE(sel.from.table_name == "my_table");
    }

    SECTION("SELECT FROM with mixed case keywords") {
        auto query  = "SeLeCt * FrOm ks.tbl;";
        auto result = parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
    }

    SECTION("SELECT FROM with extra whitespace") {
        auto query  = "SELECT   *   FROM   ks.users  ;";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& sel = get<Select>(result->value);
        REQUIRE(*sel.from.keyspace_name == "ks");
        REQUIRE(sel.from.table_name == "users");
    }

    SECTION("SELECT FROM with leading/trailing whitespace") {
        auto query  = "  SELECT * FROM ks.users;  ";
        auto result = parse(query);

        REQUIRE(result.has_value());
        const auto& sel = get<Select>(result->value);
        REQUIRE(*sel.from.keyspace_name == "ks");
        REQUIRE(sel.from.table_name == "users");
    }
}

TEST_CASE("CQL Invalid syntax handling", "[cql.parser]") {
    SECTION("Invalid keyword") {
        auto query  = "INVALID STATEMENT;";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing table name in CREATE TABLE") {
        auto query  = "CREATE TABLE;";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("SELECT with column selection") {
        // @todo
    }

    SECTION("Missing parentheses in CREATE TABLE") {
        auto query  = "CREATE TABLE ks.users id int PRIMARY KEY;";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing WITH in CREATE KEYSPACE") {
        auto query  = "CREATE KEYSPACE ks replication = 'test';";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Empty query") {
        auto query  = "";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Only whitespace") {
        auto query  = "   \n\t  ";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Unclosed string in INSERT") {
        auto query  = "INSERT INTO ks.tbl VALUES ('unclosed);";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing comma between columns") {
        auto query  = "CREATE TABLE ks.test (id int PRIMARY KEY name text);";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing comma between values") {
        auto query  = "INSERT INTO ks.tbl VALUES (1 2 3);";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Invalid data type") {
        auto query  = "CREATE TABLE ks.test (id invalidtype PRIMARY KEY);";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing closing parenthesis in INSERT") {
        auto query  = "INSERT INTO ks.tbl VALUES (1, 2, 3;";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing closing parenthesis in CREATE TABLE") {
        auto query  = "CREATE TABLE ks.test (id int PRIMARY KEY;";
        auto result = parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("select json") {
        auto result = parse("SELECT JSON * FROM tbl;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.transform.has_value());
    }
}

TEST_CASE("CQL UPDATE", "[cql.cql]") {
    auto result = parse("UPDATE tbl SET name = 'new' WHERE id = 1;");
    REQUIRE(result.has_value());
    auto& stmt = get<Update>(result->value);
    REQUIRE(stmt.table.table_name == "tbl");
    REQUIRE(stmt.assignments.length == 1);
}

TEST_CASE("CQL DELETE", "[cql.cql]") {
    auto result = parse("DELETE FROM tbl WHERE id = 1;");
    REQUIRE(result.has_value());
    auto& stmt = get<Delete>(result->value);
    REQUIRE(stmt.table.table_name == "tbl");
}

TEST_CASE("CQL case insensitive keywords", "[cql.cql]") {
    REQUIRE(parse("select * from tbl;").has_value());
    REQUIRE(parse("Select * From tbl;").has_value());
    REQUIRE(parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'};").has_value());
}

TEST_CASE("Parse USE statement", "[cql.parser]") {
    SECTION("Basic USE") {
        auto result = parse("USE my_keyspace;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<UseKeyspace>(result->value));
        const auto& stmt = get<UseKeyspace>(result->value);
        REQUIRE(stmt.keyspace == "my_keyspace");
    }
}

TEST_CASE("Parse DROP statements", "[cql.parser]") {
    SECTION("DROP KEYSPACE") {
        auto result = parse("DROP KEYSPACE my_keyspace;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropKeyspace>(result->value));
        const auto& stmt = get<DropKeyspace>(result->value);
        REQUIRE(stmt.keyspace == "my_keyspace");
        REQUIRE_FALSE(stmt.if_exists);
    }

    SECTION("DROP KEYSPACE IF EXISTS") {
        auto result = parse("DROP KEYSPACE IF EXISTS my_keyspace;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropKeyspace>(result->value));
        const auto& stmt = get<DropKeyspace>(result->value);
        REQUIRE(stmt.keyspace == "my_keyspace");
        REQUIRE(stmt.if_exists);
    }

    SECTION("DROP TABLE") {
        auto result = parse("DROP TABLE ks.my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropTable>(result->value));
        const auto& stmt = get<DropTable>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "my_table");
        REQUIRE_FALSE(stmt.if_exists);
    }

    SECTION("DROP TABLE IF EXISTS") {
        auto result = parse("DROP TABLE IF EXISTS my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropTable>(result->value));
        const auto& stmt = get<DropTable>(result->value);
        REQUIRE(!stmt.table.keyspace_name);
        REQUIRE(stmt.table.table_name == "my_table");
        REQUIRE(stmt.if_exists);
    }
}

TEST_CASE("Parse TRUNCATE statement", "[cql.parser]") {
    SECTION("TRUNCATE with keyspace") {
        auto result = parse("TRUNCATE ks.my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<TruncateTable>(result->value));
        const auto& stmt = get<TruncateTable>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "my_table");
    }

    SECTION("TRUNCATE TABLE with keyspace") {
        auto result = parse("TRUNCATE TABLE my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<TruncateTable>(result->value));
        const auto& stmt = get<TruncateTable>(result->value);
        REQUIRE(!stmt.table.keyspace_name);
        REQUIRE(stmt.table.table_name == "my_table");
    }
}

TEST_CASE("Parse UPDATE statement", "[cql.parser]") {
    SECTION("Basic UPDATE") {
        auto result = parse("UPDATE ks.users SET name = 'Alice' WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Update>(result->value));
        const auto& stmt = get<Update>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("UPDATE multiple assignments") {
        auto result = parse("UPDATE ks.users SET name = 'Alice', age = 30 WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Update>(result->value));
        [[maybe_unused]] const auto& stmt = get<Update>(result->value);
        // @todo
    }
}

TEST_CASE("Parse DELETE statement", "[cql.parser]") {
    SECTION("DELETE all columns") {
        auto result = parse("DELETE FROM ks.users WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Delete>(result->value));
        const auto& stmt = get<Delete>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("DELETE specific columns") {
        auto result = parse("DELETE name, age FROM users WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Delete>(result->value));
        [[maybe_unused]] const auto& stmt = get<Delete>(result->value);
        // @todo
    }
}

TEST_CASE("Parse SELECT with WHERE and LIMIT", "[cql.parser]") {
    SECTION("SELECT with WHERE") {
        auto result = parse("SELECT * FROM ks.users WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(*stmt.from.keyspace_name == "ks");
        REQUIRE(stmt.from.table_name == "users");
        // @todo
    }

    SECTION("SELECT with LIMIT") {
        auto result = parse("SELECT * FROM ks.users LIMIT 10;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(get<S64>(stmt.limit.value) == 10);
    }

    SECTION("SELECT with WHERE and LIMIT") {
        auto result = parse("SELECT * FROM users WHERE active = true LIMIT 5;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(!stmt.from.keyspace_name);
        REQUIRE(stmt.from.table_name == "users");
        REQUIRE(get<S64>(stmt.limit.value) == 5);
        // @todo
    }

    SECTION("SELECT specific columns") {
        auto result = parse("SELECT id, name, age FROM ks.users;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("SELECT multiple WHERE conditions") {
        auto result = parse("SELECT * FROM ks.users WHERE id = 1 AND name = 'Alice';");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }
}

TEST_CASE("Parse INSERT with column names", "[cql.parser]") {
    SECTION("INSERT with column list") {
        auto result = parse("INSERT INTO ks.users (id, name) VALUES (1, 'Alice');");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
        const auto& stmt = get<Insert>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("INSERT without column list") {
        auto result = parse("INSERT INTO users VALUES (1, 'Alice', 30);");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
        const auto& stmt = get<Insert>(result->value);
        REQUIRE(!stmt.table.keyspace_name);
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("INSERT IF NOT EXISTS") {
        auto result = parse("INSERT INTO ks.users (id) VALUES (1) IF NOT EXISTS;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
        const auto& stmt = get<Insert>(result->value);
        REQUIRE(stmt.if_not_exists);
    }
}

TEST_CASE("Parse SELECT with ORDER BY", "[cql.parser]") {
    SECTION("ORDER BY single column ascending") {
        auto result = parse("SELECT * FROM ks.users ORDER BY created_at ASC;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("ORDER BY single column descending") {
        auto result = parse("SELECT * FROM ks.users ORDER BY created_at DESC;");
        REQUIRE(result.has_value());
        const auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.order_by->columns.length == 1);
        REQUIRE(stmt.order_by->columns[0].sort == Sort::DESC);
    }

    SECTION("ORDER BY default ascending") {
        auto result = parse("SELECT * FROM ks.users ORDER BY name;");
        REQUIRE(result.has_value());
        const auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.order_by->columns.length == 1);
        REQUIRE(stmt.order_by->columns[0].sort == Sort::ASC);
        REQUIRE(stmt.order_by->columns[0].column.identifier == "name");
    }

    SECTION("ORDER BY with WHERE and LIMIT") {
        auto result = parse("SELECT * FROM ks.users WHERE id = 1 ORDER BY created_at DESC LIMIT 10;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }
}

TEST_CASE("Parse SELECT with ALLOW FILTERING", "[cql.parser]") {
    SECTION("Basic ALLOW FILTERING") {
        auto result = parse("SELECT * FROM ks.users WHERE age = 25 ALLOW FILTERING;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.allow_filtering == true);
    }

    SECTION("ALLOW FILTERING with ORDER BY and LIMIT") {
        auto result = parse("SELECT * FROM ks.users WHERE age = 25 ORDER BY name LIMIT 100 ALLOW FILTERING;");
        REQUIRE(result.has_value());
        const auto& stmt = get<Select>(result->value);
        REQUIRE(get<S64>(stmt.limit.value) == 100);
        REQUIRE(stmt.allow_filtering == true);
        // @todo
    }
}

TEST_CASE("Parse SELECT with GROUP BY", "[cql.parser]") {
    SECTION("GROUP BY single column") {
        auto result = parse("SELECT user_id FROM ks.events GROUP BY user_id;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("GROUP BY multiple columns") {
        auto result = parse("SELECT * FROM ks.events GROUP BY year, month, day;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("GROUP BY with WHERE and ORDER BY") {
        auto result = parse("SELECT * FROM ks.events WHERE user_id = 1 GROUP BY event_type ORDER BY created_at DESC;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("GROUP BY with LIMIT and ALLOW FILTERING") {
        auto result = parse("SELECT * FROM ks.events GROUP BY user_id LIMIT 50 ALLOW FILTERING;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }
}

TEST_CASE("Parse CREATE KEYSPACE with map literal replication", "[cql.parser]") {
    SECTION("Simple map replication") {
        auto result = parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'};");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 1);
        REQUIRE(ks.options.identifier_values[0].first == "replication");
        REQUIRE(type_matches_tag<MapLiteral>(ks.options.identifier_values[0].second));
        [[maybe_unused]] const auto& map = get<MapLiteral>(ks.options.identifier_values[0].second);
        // @todo
    }

    SECTION("Map with multiple entries") {
        auto result = parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 3};");
        REQUIRE(result.has_value());
        const auto&                  ks  = get<CreateKeyspace>(result->value);
        [[maybe_unused]] const auto& map = get<MapLiteral>(ks.options.identifier_values[0].second);
        // @todo
    }

    SECTION("NetworkTopologyStrategy with datacenter configs") {
        auto result = parse("CREATE KEYSPACE ks WITH replication = {'class': 'NetworkTopologyStrategy', 'dc1': 3, 'dc2': 2};");
        REQUIRE(result.has_value());
        const auto&                  ks  = get<CreateKeyspace>(result->value);
        [[maybe_unused]] const auto& map = get<MapLiteral>(ks.options.identifier_values[0].second);
        // @todo
    }

    SECTION("Mix of map and scalar options") {
        auto result = parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'} AND durable_writes = 'true';");
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 2);
        REQUIRE(type_matches_tag<MapLiteral>(ks.options.identifier_values[0].second));
        // @note quoted option values are stored as Constant{AutoString8}
        REQUIRE(type_matches_tag<Constant>(ks.options.identifier_values[1].second));
        REQUIRE(get<AutoString8>(get<Constant>(ks.options.identifier_values[1].second).value) == "true");
    }
}

TEST_CASE("CQL parse error reporting", "[cql.parser]") {
    SECTION("Invalid syntax returns empty with Catch2 reporter") {
        auto result = parse("INVALID STATEMENT;");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Valid query succeeds with Catch2 reporter") {
        auto result = parse("SELECT * FROM ks.tbl;");
        REQUIRE(result.has_value());
    }

    SECTION("Empty query returns empty with Catch2 reporter") {
        auto result = parse("");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Unclosed string returns empty with Catch2 reporter") {
        auto result = parse("INSERT INTO ks.tbl VALUES ('unclosed);");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Invalid syntax returns empty with bool reporter") {
        auto result = parse("CREATE TABLE;");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("CQL quoted identifiers", "[cql.cql]") {
    auto result = parse("SELECT * FROM \"MyTable\";");
    REQUIRE(result.has_value());
    auto& stmt = get<Select>(result->value);
    REQUIRE(stmt.from.table_name == "MyTable");
}

TEST_CASE("CQL string escape", "[cql.cql]") {
    auto result = parse("INSERT INTO tbl (id, name) VALUES (1, 'it''s');");
    REQUIRE(result.has_value());
    auto& stmt    = get<Insert>(result->value);
    auto& nv      = get<Insert::NamesValues>(stmt.insert_clause);
    auto& str_val = get<AutoString8>(get<Constant>(nv.values[1].value).value);
    REQUIRE(str_val == "it's");
}

TEST_CASE("Conformance: CREATE INDEX", "[cql.conformance.parser]") {
    SECTION("anonymous index on column") {
        auto r = parse("CREATE INDEX ON ks.tbl(categories);");
        REQUIRE(r.has_value());
        auto& s = get<CreateIndex>(r->value);
        REQUIRE(!s.custom);
        REQUIRE(!s.if_not_exists);
        REQUIRE(!s.index_name);
        REQUIRE(s.table.table_name == "tbl");
    }
    SECTION("named index on column") {
        auto r = parse("CREATE INDEX v_idx_1 ON ks.tbl(v);");
        REQUIRE(r.has_value());
        auto& s = get<CreateIndex>(r->value);
        REQUIRE(!s.custom);
        REQUIRE(s.index_name.has_value());
        REQUIRE(*s.index_name == "v_idx_1");
        REQUIRE(s.table.table_name == "tbl");
    }
    SECTION("custom index") {
        auto r = parse("CREATE CUSTOM INDEX ON ks.tbl(col);");
        REQUIRE(r.has_value());
        auto& s = get<CreateIndex>(r->value);
        REQUIRE(s.custom);
    }
    SECTION("if not exists") {
        auto r = parse("CREATE INDEX IF NOT EXISTS ON ks.tbl(col);");
        REQUIRE(r.has_value());
        auto& s = get<CreateIndex>(r->value);
        REQUIRE(s.if_not_exists);
        REQUIRE(!s.index_name);
    }
    SECTION("named index if not exists") {
        auto r = parse("CREATE INDEX IF NOT EXISTS idx_name ON ks.tbl(col);");
        REQUIRE(r.has_value());
        auto& s = get<CreateIndex>(r->value);
        REQUIRE(s.if_not_exists);
        REQUIRE(s.index_name.has_value());
        REQUIRE(*s.index_name == "idx_name");
    }
    SECTION("function column specifier keys()") {
        auto r = parse("CREATE INDEX ON ks.tbl(keys(categories));");
        REQUIRE(r.has_value());
        REQUIRE(type_matches_tag<CreateIndex>(r->value));
    }
    SECTION("unqualified table name") {
        auto r = parse("CREATE INDEX ON tbl(col);");
        REQUIRE(r.has_value());
        auto& s = get<CreateIndex>(r->value);
        REQUIRE(!s.table.keyspace_name);
        REQUIRE(s.table.table_name == "tbl");
    }
}

TEST_CASE("Conformance: CREATE TYPE", "[cql.conformance.parser]") {
    SECTION("single field") {
        auto r = parse("CREATE TYPE ks.my_type(v1 int);");
        REQUIRE(r.has_value());
        auto& s = get<CreateType>(r->value);
        REQUIRE(s.name.table_name == "my_type");
        REQUIRE(*s.name.keyspace_name == "ks");
        REQUIRE(!s.if_not_exists);
        REQUIRE(s.fields.length == 1);
        REQUIRE(s.fields[0].name.identifier == "v1");
        REQUIRE(s.fields[0].type == type::create_basic(type::Basic::int_));
    }
    SECTION("multiple fields") {
        auto r = parse("CREATE TYPE ks.address(street text, city text, zip int);");
        REQUIRE(r.has_value());
        auto& s = get<CreateType>(r->value);
        REQUIRE(s.fields.length == 3);
        REQUIRE(s.fields[0].name.identifier == "street");
        REQUIRE(s.fields[1].name.identifier == "city");
        REQUIRE(s.fields[2].name.identifier == "zip");
    }
    SECTION("if not exists") {
        auto r = parse("CREATE TYPE IF NOT EXISTS ks.my_type(a int, b text);");
        REQUIRE(r.has_value());
        auto& s = get<CreateType>(r->value);
        REQUIRE(s.if_not_exists);
        REQUIRE(s.fields.length == 2);
    }
    SECTION("alter_test shape single int field") {
        auto r = parse("CREATE TYPE ks.t1(v1 int);");
        REQUIRE(r.has_value());
        REQUIRE(type_matches_tag<CreateType>(r->value));
    }
    SECTION("DROP TYPE IF EXISTS") {
        auto r = parse("DROP TYPE IF EXISTS ks.type_does_not_exist;");
        REQUIRE(r.has_value());
        REQUIRE(type_matches_tag<DropType>(r->value));
    }
}

TEST_CASE("Conformance: ALTER TABLE DROP with USING TIMESTAMP", "[cql.conformance.parser]") {
    SECTION("single column DROP with USING TIMESTAMP") {
        auto r = parse("ALTER TABLE ks.tbl DROP todrop USING TIMESTAMP 20000;");
        REQUIRE(r.has_value());
        auto& s     = get<AlterTable>(r->value);
        auto& instr = get<AlterTable::DropColumnInstruction>(s.alter_table_instruction);
        REQUIRE(instr.columns.length == 1);
        REQUIRE(instr.columns[0].identifier == "todrop");
    }
    SECTION("parenthesized multi-column DROP with USING TIMESTAMP") {
        auto r = parse("ALTER TABLE ks.tbl DROP (todrop1, todrop2) USING TIMESTAMP 20000;");
        REQUIRE(r.has_value());
        auto& s     = get<AlterTable>(r->value);
        auto& instr = get<AlterTable::DropColumnInstruction>(s.alter_table_instruction);
        REQUIRE(instr.columns.length == 2);
        REQUIRE(instr.columns[0].identifier == "todrop1");
        REQUIRE(instr.columns[1].identifier == "todrop2");
    }
    SECTION("DROP without USING TIMESTAMP") {
        auto r = parse("ALTER TABLE ks.tbl DROP myCollection;");
        REQUIRE(r.has_value());
        auto& s     = get<AlterTable>(r->value);
        auto& instr = get<AlterTable::DropColumnInstruction>(s.alter_table_instruction);
        REQUIRE(instr.columns.length == 1);
    }
    SECTION("multiple columns DROP without parens") {
        auto r = parse("ALTER TABLE ks.tbl DROP col1, col2;");
        REQUIRE(r.has_value());
        auto& s     = get<AlterTable>(r->value);
        auto& instr = get<AlterTable::DropColumnInstruction>(s.alter_table_instruction);
        REQUIRE(instr.columns.length == 2);
    }
    SECTION("ALTER TABLE ADD with list<text>") {
        auto r = parse("ALTER TABLE ks.tbl ADD myCollection list<text>;");
        REQUIRE(r.has_value());
        auto& s = get<AlterTable>(r->value);
        REQUIRE(type_matches_tag<AlterTable::AddColumnInstruction>(s.alter_table_instruction));
        auto& instr = get<AlterTable::AddColumnInstruction>(s.alter_table_instruction);
        REQUIRE(instr.column_definitions.length == 1);
    }
    SECTION("ALTER TABLE ADD with map<text, text>") {
        auto r = parse("ALTER TABLE ks.tbl ADD myCollection map<text, text>;");
        REQUIRE(r.has_value());
        REQUIRE(type_matches_tag<AlterTable::AddColumnInstruction>(
            get<AlterTable>(r->value).alter_table_instruction));
    }
}

TEST_CASE("Conformance: collection literals in INSERT VALUES", "[cql.conformance.parser]") {
    SECTION("list literal in VALUES") {
        auto r = parse("INSERT INTO tbl (k, l, c) VALUES (3, [0, 1, 2], 4);");
        REQUIRE(r.has_value());
        auto& s  = get<Insert>(r->value);
        auto& nv = get<Insert::NamesValues>(s.insert_clause);
        REQUIRE(nv.values.length == 3);
        REQUIRE(type_matches_tag<ListOrVectorLiteral>(nv.values[1].value));
    }
    SECTION("list, map and set literals in VALUES") {
        auto r = parse("INSERT INTO tbl (a, b, c, d, e, f) VALUES (1, 1, 1, [1, 2], {1: 2}, {1, 2});");
        REQUIRE(r.has_value());
        auto& s  = get<Insert>(r->value);
        auto& nv = get<Insert::NamesValues>(s.insert_clause);
        REQUIRE(nv.values.length == 6);
        REQUIRE(type_matches_tag<ListOrVectorLiteral>(nv.values[3].value));
        REQUIRE(type_matches_tag<MapLiteral>(nv.values[4].value));
        REQUIRE(type_matches_tag<SetLiteral>(nv.values[5].value));
    }
    SECTION("set literal with single element") {
        auto r = parse("INSERT INTO tbl (k, s) VALUES (1, {1});");
        REQUIRE(r.has_value());
        auto& s  = get<Insert>(r->value);
        auto& nv = get<Insert::NamesValues>(s.insert_clause);
        REQUIRE(nv.values.length == 2);
        REQUIRE(type_matches_tag<SetLiteral>(nv.values[1].value));
    }
    SECTION("list with function call element") {
        auto r = parse("INSERT INTO tbl (k, v) VALUES (0, [now()]);");
        REQUIRE(r.has_value());
        auto& s  = get<Insert>(r->value);
        auto& nv = get<Insert::NamesValues>(s.insert_clause);
        REQUIRE(type_matches_tag<ListOrVectorLiteral>(nv.values[1].value));
    }
    SECTION("list literal in positional VALUES") {
        auto r = parse("INSERT INTO tbl VALUES (1, [1, 2, 3]);");
        REQUIRE(r.has_value());
        auto& s  = get<Insert>(r->value);
        auto& nv = get<Insert::NamesValues>(s.insert_clause);
        REQUIRE(nv.values.length == 2);
        REQUIRE(type_matches_tag<ListOrVectorLiteral>(nv.values[1].value));
    }
}

TEST_CASE("Conformance: USING TTL AND TIMESTAMP combined", "[cql.conformance.parser]") {
    SECTION("INSERT USING TTL then TIMESTAMP literal") {
        auto r = parse("INSERT INTO ks.tbl (id, name) VALUES (1, 'a') USING TTL 1000 AND TIMESTAMP 0;");
        REQUIRE(r.has_value());
        auto& s = get<Insert>(r->value);
        REQUIRE(s.using_parameters.length == 2);
        bool has_ttl = false, has_ts = false;
        for (U64 i = 0; i < s.using_parameters.length; ++i) {
            if (s.using_parameters[i].kind == UpdateParameter::Kind::TTL) {
                has_ttl = true;
            }
            if (s.using_parameters[i].kind == UpdateParameter::Kind::TIMESTAMP) {
                has_ts = true;
            }
        }
        REQUIRE(has_ttl);
        REQUIRE(has_ts);
    }
    SECTION("INSERT USING TIMESTAMP then TTL bind markers") {
        auto r = parse("INSERT INTO ks.tbl (k, c, i) VALUES (?, ?, ?) USING TIMESTAMP ? AND TTL ?;");
        REQUIRE(r.has_value());
        auto& s = get<Insert>(r->value);
        REQUIRE(s.using_parameters.length == 2);
    }
    SECTION("UPDATE USING TIMESTAMP then TTL bind markers") {
        auto r = parse("UPDATE ks.tbl USING TIMESTAMP ? AND TTL ? SET i = 1 WHERE k = 1;");
        REQUIRE(r.has_value());
        auto& s = get<Update>(r->value);
        REQUIRE(s.using_parameters.length == 2);
    }
    SECTION("INSERT USING TTL only") {
        auto r = parse("INSERT INTO ks.tbl (k, v) VALUES (1, 1) USING TTL 300;");
        REQUIRE(r.has_value());
        auto& s = get<Insert>(r->value);
        REQUIRE(s.using_parameters.length == 1);
        REQUIRE(s.using_parameters[0].kind == UpdateParameter::Kind::TTL);
    }
    SECTION("INSERT USING TIMESTAMP only") {
        auto r = parse("INSERT INTO ks.tbl (k, v) VALUES (1, 1) USING TIMESTAMP 12345;");
        REQUIRE(r.has_value());
        auto& s = get<Insert>(r->value);
        REQUIRE(s.using_parameters.length == 1);
        REQUIRE(s.using_parameters[0].kind == UpdateParameter::Kind::TIMESTAMP);
    }
}

TEST_CASE("Conformance: SELECT with distinct/json as column names", "[cql.conformance.parser]") {
    SECTION("both distinct and json as column names") {
        auto r = parse("SELECT distinct, json FROM tbl;");
        REQUIRE(r.has_value());
        auto& s = get<Select>(r->value);
        REQUIRE(!s.transform.has_value());
        REQUIRE(s.select.clauses.length == 2);
    }
    // "SELECT distinct distinct" = SELECT DISTINCT <col named distinct>; first keyword is the transform.
    SECTION("SELECT DISTINCT with column named distinct") {
        auto r = parse("SELECT distinct distinct FROM tbl;");
        REQUIRE(r.has_value());
        auto& s = get<Select>(r->value);
        REQUIRE(s.transform.has_value());
        REQUIRE(*s.transform == Select::Transform::UNIQUE);
        REQUIRE(s.select.clauses.length == 1);
    }
    SECTION("SELECT JSON transform still works") {
        auto r = parse("SELECT JSON * FROM tbl;");
        REQUIRE(r.has_value());
        auto& s = get<Select>(r->value);
        REQUIRE(s.transform.has_value());
        REQUIRE(*s.transform == Select::Transform::JSON);
    }
    SECTION("SELECT DISTINCT transform still works") {
        auto r = parse("SELECT DISTINCT * FROM tbl;");
        REQUIRE(r.has_value());
        auto& s = get<Select>(r->value);
        REQUIRE(s.transform.has_value());
        REQUIRE(*s.transform == Select::Transform::UNIQUE);
    }
}

TEST_CASE("Conformance: tuple<> type in CREATE TABLE", "[cql.conformance.parser]") {
    SECTION("two-element tuple column") {
        auto r = parse("CREATE TABLE ks.tbl (pk int PRIMARY KEY, t tuple<int, duration>);");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.column_definitions.length == 2);
        auto& col = s.column_definitions[1];
        REQUIRE(col.name.identifier == "t");
        REQUIRE(type_matches_tag<type::Tuple>(col.type.value));
        auto& tup = get<type::Tuple>(col.type.value);
        REQUIRE(tup.elements.length == 2);
        REQUIRE(tup.frozen == false);
        REQUIRE(type_matches_tag<type::Basic>(tup.elements[0].value));
        REQUIRE(get<type::Basic>(tup.elements[0].value) == type::Basic::int_);
        REQUIRE(type_matches_tag<type::Basic>(tup.elements[1].value));
        REQUIRE(get<type::Basic>(tup.elements[1].value) == type::Basic::duration);
    }
    SECTION("frozen tuple as PRIMARY KEY column") {
        auto r = parse("CREATE TABLE ks.tbl (t frozen<tuple<int, duration>> PRIMARY KEY, v int);");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.column_definitions.length == 2);
        auto& col = s.column_definitions[0];
        REQUIRE(col.name.identifier == "t");
        REQUIRE(type_matches_tag<type::Tuple>(col.type.value));
        auto& tup = get<type::Tuple>(col.type.value);
        REQUIRE(tup.elements.length == 2);
        REQUIRE(tup.frozen == true);
    }
    SECTION("single-element tuple") {
        auto r = parse("CREATE TABLE ks.tbl (pk int PRIMARY KEY, t tuple<text>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Tuple>(col.type.value));
        REQUIRE(get<type::Tuple>(col.type.value).elements.length == 1);
    }
    SECTION("three-element tuple") {
        auto r = parse("CREATE TABLE ks.tbl (pk int PRIMARY KEY, t tuple<int, text, boolean>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Tuple>(col.type.value));
        REQUIRE(get<type::Tuple>(col.type.value).elements.length == 3);
    }
    SECTION("tuple with collection element") {
        auto r = parse("CREATE TABLE ks.tbl (pk int PRIMARY KEY, t tuple<int, list<text>>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Tuple>(col.type.value));
        auto& tup = get<type::Tuple>(col.type.value);
        REQUIRE(tup.elements.length == 2);
        REQUIRE(type_matches_tag<type::List>(tup.elements[1].value));
    }
    SECTION("CREATE TYPE with tuple field") {
        auto r = parse("CREATE TYPE ks.my_type (a tuple<int, text>);");
        REQUIRE(r.has_value());
        auto& s = get<CreateType>(r->value);
        REQUIRE(s.fields.length == 1);
        REQUIRE(type_matches_tag<type::Tuple>(s.fields[0].type.value));
        REQUIRE(get<type::Tuple>(s.fields[0].type.value).elements.length == 2);
    }
    SECTION("frozen<map<text, list<tuple<int, duration>>>> nested type") {
        auto r = parse("CREATE TABLE ks.tbl (pk int, m frozen<map<text, list<tuple<int, duration>>>>, v int, PRIMARY KEY (pk, m));");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Map>(col.type.value));
        auto& m = get<type::Map>(col.type.value);
        REQUIRE(m.frozen == true);
        REQUIRE(type_matches_tag<type::Basic>(m.key.value));
        REQUIRE(get<type::Basic>(m.key.value) == type::Basic::text);
        REQUIRE(type_matches_tag<type::List>(m.value.value));
        auto& l = get<type::List>(m.value.value);
        REQUIRE(type_matches_tag<type::Tuple>(l.element.value));
        REQUIRE(get<type::Tuple>(l.element.value).elements.length == 2);
    }
    SECTION("frozen<set<tuple<int, text, double>>> nested type") {
        auto r = parse("CREATE TABLE ks.tbl (k int PRIMARY KEY, s frozen<set<tuple<int, text, double>>>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Set>(col.type.value));
        auto& st = get<type::Set>(col.type.value);
        REQUIRE(st.frozen == true);
        REQUIRE(type_matches_tag<type::Tuple>(st.key.value));
        REQUIRE(get<type::Tuple>(st.key.value).elements.length == 3);
    }
    SECTION("map<text, frozen<map<text, set<int>>>> nested type") {
        auto r = parse("CREATE TABLE ks.tbl (k int PRIMARY KEY, m map<text, frozen<map<text, set<int>>>>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Map>(col.type.value));
        auto& outer = get<type::Map>(col.type.value);
        REQUIRE(type_matches_tag<type::Basic>(outer.key.value));
        REQUIRE(type_matches_tag<type::Map>(outer.value.value));
        REQUIRE(get<type::Map>(outer.value.value).frozen == true);
    }
}

TEST_CASE("check_specific_errors", "[cql.cql]") {
    SECTION("USE with positional bind marker") {
        auto err = check_specific_errors("USE ?");
        REQUIRE(err.has_value());
        REQUIRE(String8(*err) == String8{"Bind variables cannot be used for keyspace names"});
    }
    SECTION("USE with named bind marker") {
        auto err = check_specific_errors("USE :name");
        REQUIRE(err.has_value());
    }
    SECTION("USE with leading whitespace") {
        auto err = check_specific_errors("  USE ?");
        REQUIRE(err.has_value());
    }
    SECTION("USE case-insensitive") {
        auto err = check_specific_errors("use ?");
        REQUIRE(err.has_value());
    }
    SECTION("valid USE returns empty") {
        auto err = check_specific_errors("USE my_keyspace");
        REQUIRE(!err.has_value());
    }
    SECTION("non-USE query returns empty") {
        auto err = check_specific_errors("SELECT * FROM t WHERE k = ?");
        REQUIRE(!err.has_value());
    }
}

TEST_CASE("CREATE TABLE trailing comma", "[cql.cql]") {
    SECTION("trailing comma after last column") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY, v int,);");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.column_definitions.length == 2);
    }
    SECTION("trailing comma after standalone PRIMARY KEY clause") {
        auto r = parse("CREATE TABLE ks.t (k int, c int, PRIMARY KEY (k),);");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.column_definitions.length == 2);
        REQUIRE(s.primary_key.has_value());
    }
}

TEST_CASE("ALTER TYPE", "[cql.cql]") {
    SECTION("ADD field") {
        auto r = parse("ALTER TYPE ks.mytype ADD v2 int;");
        REQUIRE(r.has_value());
        REQUIRE(type_matches_tag<AlterType>(r->value));
        auto& s = get<AlterType>(r->value);
        REQUIRE(type_matches_tag<AlterType::AddFieldInstruction>(s.instruction));
        REQUIRE(get<AlterType::AddFieldInstruction>(s.instruction).fields.length == 1);
    }
    SECTION("RENAME field") {
        auto r = parse("ALTER TYPE ks.mytype RENAME v1 TO v1_renamed;");
        REQUIRE(r.has_value());
        REQUIRE(type_matches_tag<AlterType>(r->value));
        auto& s = get<AlterType>(r->value);
        REQUIRE(type_matches_tag<AlterType::RenameFieldInstruction>(s.instruction));
    }
}

TEST_CASE("composite partition key parsing", "[cql.parser]") {
    SECTION("compound partition key with single clustering column") {
        auto r = parse("CREATE TABLE ks.t (a int, b text, c int, PRIMARY KEY ((a, b), c));");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.primary_key.has_value());
        REQUIRE(type_matches_tag<DynamicArray<ColumnName>>(s.primary_key->partition_key.column_or_columns));
        auto& pk_cols = get<DynamicArray<ColumnName>>(s.primary_key->partition_key.column_or_columns);
        REQUIRE(pk_cols.length == 2);
        REQUIRE(pk_cols[0].identifier == "a");
        REQUIRE(pk_cols[1].identifier == "b");
        REQUIRE(s.primary_key->clustering_columns.length == 1);
        REQUIRE(s.primary_key->clustering_columns[0].identifier == "c");
    }
    SECTION("compound partition key only (no clustering)") {
        auto r = parse("CREATE TABLE ks.t (a int, b int, PRIMARY KEY ((a, b)));");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.primary_key.has_value());
        REQUIRE(type_matches_tag<DynamicArray<ColumnName>>(s.primary_key->partition_key.column_or_columns));
        auto& pk_cols = get<DynamicArray<ColumnName>>(s.primary_key->partition_key.column_or_columns);
        REQUIRE(pk_cols.length == 2);
        REQUIRE(s.primary_key->clustering_columns.length == 0);
    }
    SECTION("three-column compound partition key") {
        auto r = parse("CREATE TABLE ks.t (a int, b int, c int, PRIMARY KEY ((a, b, c)));");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.primary_key.has_value());
        auto& pk_cols = get<DynamicArray<ColumnName>>(s.primary_key->partition_key.column_or_columns);
        REQUIRE(pk_cols.length == 3);
    }
    SECTION("single partition key (non-compound) is still a ColumnName not array") {
        auto r = parse("CREATE TABLE ks.t (k int, PRIMARY KEY (k));");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.primary_key.has_value());
        REQUIRE(type_matches_tag<ColumnName>(s.primary_key->partition_key.column_or_columns));
    }
}

TEST_CASE("frozen collection type parsing", "[cql.parser]") {
    SECTION("FROZEN<LIST<INT>>") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY, v frozen<list<int>>);");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.column_definitions.length == 2);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::List>(col.type.value));
        REQUIRE(get<type::List>(col.type.value).frozen == true);
        REQUIRE(type_matches_tag<type::Basic>(get<type::List>(col.type.value).element.value));
        REQUIRE(get<type::Basic>(get<type::List>(col.type.value).element.value) == type::Basic::int_);
    }
    SECTION("FROZEN<SET<TEXT>>") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY, v frozen<set<text>>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Set>(col.type.value));
        REQUIRE(get<type::Set>(col.type.value).frozen == true);
    }
    SECTION("FROZEN<MAP<TEXT, INT>>") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY, v frozen<map<text, int>>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::Map>(col.type.value));
        REQUIRE(get<type::Map>(col.type.value).frozen == true);
    }
    SECTION("non-frozen list parses with frozen=false") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY, v list<int>);");
        REQUIRE(r.has_value());
        auto& s   = get<CreateTable>(r->value);
        auto& col = s.column_definitions[1];
        REQUIRE(type_matches_tag<type::List>(col.type.value));
        REQUIRE(get<type::List>(col.type.value).frozen == false);
    }
}

TEST_CASE("CREATE TABLE WITH options parsing", "[cql.parser]") {
    SECTION("unknown option key is parsed without error") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY) WITH some_future_option = 'value';");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.options.value.length == 1);
    }
    SECTION("default_time_to_live option is parsed") {
        auto r = parse("CREATE TABLE ks.t (k int PRIMARY KEY) WITH default_time_to_live = 3600;");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.options.value.length == 1);
    }
    SECTION("multiple mixed options are all parsed") {
        auto r = parse(
            "CREATE TABLE ks.t (k int PRIMARY KEY) "
            "WITH compaction = {'class': 'SizeTieredCompactionStrategy'} "
            "AND compression = {'sstable_compression': 'LZ4Compressor'} "
            "AND gc_grace_seconds = 864000;");
        REQUIRE(r.has_value());
        auto& s = get<CreateTable>(r->value);
        REQUIRE(s.options.value.length == 3);
    }
}
