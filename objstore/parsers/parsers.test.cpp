#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

import plexdb.base;
import plexdb.tagged_union;
import plexdb.dynamic.tagged_union;

import objstore.parsers;
import objstore.engine.statements;
import objstore.engine.types;

using namespace plexdb;
using namespace objstore;
using namespace objstore::parsers;

// ============================================================================
// cassandra query language (CQL)
// ============================================================================

TEST_CASE("CQL CREATE KEYSPACE", "[objstore.cql]") {
    SECTION("basic") {
        auto result = cql::parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': '1'};");
        REQUIRE(result.has_value());
        auto& stmt = get<CreateKeyspace>(result->value);
        REQUIRE(stmt.name == "ks");
        REQUIRE(stmt.if_not_exists == false);
    }
    SECTION("if not exists") {
        auto result = cql::parse("CREATE KEYSPACE IF NOT EXISTS ks WITH replication = {'class': 'SimpleStrategy'};");
        REQUIRE(result.has_value());
        auto& stmt = get<CreateKeyspace>(result->value);
        REQUIRE(stmt.if_not_exists == true);
    }
}

TEST_CASE("CQL USE KEYSPACE", "[objstore.cql]") {
    auto result = cql::parse("USE my_keyspace;");
    REQUIRE(result.has_value());
    auto& stmt = get<UseKeyspace>(result->value);
    REQUIRE(stmt.keyspace == "my_keyspace");
}

TEST_CASE("CQL DROP KEYSPACE", "[objstore.cql]") {
    auto result = cql::parse("DROP KEYSPACE IF EXISTS ks;");
    REQUIRE(result.has_value());
    auto& stmt = get<DropKeyspace>(result->value);
    REQUIRE(stmt.if_exists == true);
    REQUIRE(stmt.keyspace == "ks");
}

TEST_CASE("CQL CREATE KEYSPACE statements", "[objstore.parser]") {
    SECTION("Basic CREATE KEYSPACE") {
        auto query = "CREATE KEYSPACE my_keyspace WITH replication = 'SimpleStrategy';";
        auto result = cql::parse(query);
    }
}

TEST_CASE("CQL CREATE TABLE", "[objstore.cql]") {
    SECTION("simple table with inline primary key") {
        auto result = cql::parse("CREATE TABLE ks.tbl (id int PRIMARY KEY, name text, age int);");

        REQUIRE(result.has_value());
        auto& stmt = get<CreateTable>(result->value);
        REQUIRE(stmt.name.table_name == "tbl");
        REQUIRE(stmt.column_definitions.length == 3);
    }

    SECTION("CREATE KEYSPACE IF NOT EXISTS") {
        auto query = "CREATE KEYSPACE IF NOT EXISTS test_ks WITH replication = 'NetworkTopologyStrategy';";
        auto result = cql::parse(query);

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
        auto query = "CREATE KEYSPACE prod WITH replication = 'SimpleStrategy' AND durable_writes = 'true';";
        auto result = cql::parse(query);

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
        auto query = "CREATE KEYSPACE multi_opt WITH replication = 'NetworkTopologyStrategy' AND durable_writes = 'true' AND strategy_class = 'SimpleStrategy';";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));

        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 3);
        REQUIRE(ks.options.identifier_values[0].first == "replication");
        REQUIRE(ks.options.identifier_values[1].first == "durable_writes");
        REQUIRE(ks.options.identifier_values[2].first == "strategy_class");
    }

    SECTION("CREATE KEYSPACE case insensitive") {
        auto query = "create keyspace TestKS with replication = 'test';";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspace>(result->value));

        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.name == "testks"); // @note CQL folds unquoted identifiers to lowercase
    }

    SECTION("CREATE KEYSPACE with underscore in name") {
        auto query = "CREATE KEYSPACE my_test_keyspace WITH replication = 'SimpleStrategy';";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.name == "my_test_keyspace");
    }

    SECTION("CREATE KEYSPACE with mixed case IF NOT EXISTS") {
        auto query = "CREATE KEYSPACE If Not Exists mixed_case WITH replication = 'test';";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.if_not_exists == true);
        REQUIRE(ks.name == "mixed_case");
    }

    SECTION("CREATE KEYSPACE with quoted option value") {
        // @note CQL uses '' to escape single quotes inside strings, not backslash
        auto query = "CREATE KEYSPACE ks WITH replication = '{''class'': ''SimpleStrategy''}';";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 1);
    }
}

TEST_CASE("CQL CREATE TABLE statements", "[objstore.parser]") {
    SECTION("Basic CREATE TABLE with single column") {
        auto query = "CREATE TABLE ks.users (id int PRIMARY KEY);";
        auto result = cql::parse(query);
    }

    SECTION("if not exists") {
        auto result = cql::parse("CREATE TABLE IF NOT EXISTS tbl (id int PRIMARY KEY);");
        REQUIRE(result.has_value());
        auto& stmt = get<CreateTable>(result->value);
        REQUIRE(stmt.if_not_exists == true);
    }
}

TEST_CASE("CQL DROP TABLE", "[objstore.cql]") {
    auto result = cql::parse("DROP TABLE IF EXISTS ks.tbl;");
    REQUIRE(result.has_value());
    auto& stmt = get<DropTable>(result->value);
    REQUIRE(stmt.if_exists == true);
    REQUIRE(stmt.table.table_name == "tbl");
}

TEST_CASE("CQL TRUNCATE", "[objstore.cql]") {
    auto result = cql::parse("TRUNCATE TABLE ks.tbl;");
    REQUIRE(result.has_value());
    auto& stmt = get<TruncateTable>(result->value);
    REQUIRE(stmt.table.table_name == "tbl");
}

TEST_CASE("CQL INSERT INTO", "[objstore.cql]") {
    SECTION("with column names and values") {
        auto result = cql::parse("INSERT INTO tbl (id, name) VALUES (1, 'hello');");
        REQUIRE(result.has_value());
        auto& stmt = get<Insert>(result->value);
        REQUIRE(stmt.table.table_name == "tbl");
        auto& nv = get<Insert::NamesValues>(stmt.insert_clause);
        REQUIRE(nv.names.length == 2);
        REQUIRE(nv.values.length == 2);
    }

    SECTION("CREATE TABLE with multiple columns") {
        auto query = "CREATE TABLE ks.users (id int PRIMARY KEY, name text, age int);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTable>(result->value));

        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.name.table_name == "users");
        REQUIRE(tbl.column_definitions.length == 3);

        REQUIRE(tbl.column_definitions[0].name.identifier == "id");
        REQUIRE(tbl.column_definitions[0].type == make_basic(BasicType::int_));
        REQUIRE(tbl.column_definitions[0].primary_key == true);

        REQUIRE(tbl.column_definitions[1].name.identifier == "name");
        REQUIRE(tbl.column_definitions[1].type == make_basic(BasicType::text));
        REQUIRE(tbl.column_definitions[1].primary_key == false);

        REQUIRE(tbl.column_definitions[2].name.identifier == "age");
        REQUIRE(tbl.column_definitions[2].type == make_basic(BasicType::int_));
        REQUIRE(tbl.column_definitions[2].primary_key == false);
    }

    SECTION("CREATE TABLE IF NOT EXISTS") {
        auto query = "CREATE TABLE IF NOT EXISTS ks.products (sku int PRIMARY KEY, name text, price int);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTable>(result->value));

        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.name.table_name == "products");
        REQUIRE(tbl.if_not_exists == true);
        REQUIRE(tbl.column_definitions.length == 3);
    }

    SECTION("CREATE TABLE with various data types") {
        auto query = "CREATE TABLE ks.data (id int PRIMARY KEY, name text, count bigint, created timestamp, active boolean);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 5);
        REQUIRE(tbl.column_definitions[0].type == make_basic(BasicType::int_));
        REQUIRE(tbl.column_definitions[1].type == make_basic(BasicType::text));
        REQUIRE(tbl.column_definitions[2].type == make_basic(BasicType::bigint));
        REQUIRE(tbl.column_definitions[3].type == make_basic(BasicType::timestamp));
        REQUIRE(tbl.column_definitions[4].type == make_basic(BasicType::boolean));
    }

    SECTION("CREATE TABLE with FLOAT and DOUBLE types") {
        auto query = "CREATE TABLE prod.metrics (id int PRIMARY KEY, temperature float, precision_value double);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 3);
        REQUIRE(tbl.column_definitions[1].type == make_basic(BasicType::float_));
        REQUIRE(tbl.column_definitions[2].type == make_basic(BasicType::double_));
    }

    SECTION("CREATE TABLE with UUID type") {
        auto query = "CREATE TABLE ks.sessions (session_id uuid PRIMARY KEY, user_id int);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 2);
        REQUIRE(tbl.column_definitions[0].type == make_basic(BasicType::uuid));
        REQUIRE(tbl.column_definitions[0].primary_key == true);
    }

    SECTION("CREATE TABLE case insensitive") {
        auto query = "create table ks.TestTable (Id INT primary key, Name TEXT);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTable>(result->value));

        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.name.table_name == "testtable"); // @note case folding
    }

    SECTION("CREATE TABLE with non-primary key as last column") {
        auto query = "CREATE TABLE ks.test (id int PRIMARY KEY, data text);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions[0].primary_key == true);
        REQUIRE(tbl.column_definitions[1].primary_key == false);
    }

    SECTION("CREATE TABLE with primary key in middle") {
        auto query = "CREATE TABLE ks.test (name text, id int PRIMARY KEY, email text);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 3);
        REQUIRE(tbl.column_definitions[0].primary_key == false);
        REQUIRE(tbl.column_definitions[1].primary_key == true);
        REQUIRE(tbl.column_definitions[2].primary_key == false);
    }

    SECTION("CREATE TABLE with many columns") {
        auto query = "CREATE TABLE ks.large (c1 int PRIMARY KEY, c2 text, c3 bigint, c4 timestamp, c5 boolean, c6 float, c7 double);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions.length == 7);
    }

    SECTION("CREATE TABLE with underscore column names") {
        auto query = "CREATE TABLE ks.test (user_id int PRIMARY KEY, first_name text, last_name text);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTable>(result->value);
        REQUIRE(tbl.column_definitions[0].name.identifier == "user_id");
        REQUIRE(tbl.column_definitions[1].name.identifier == "first_name");
        REQUIRE(tbl.column_definitions[2].name.identifier == "last_name");
    }
}

TEST_CASE("CQL INSERT INTO statements", "[objstore.parser]") {
    SECTION("INSERT INTO with integer values") {
        auto query = "INSERT INTO ks.users VALUES (1, 2, 3);";
        auto result = cql::parse(query);
    }

    SECTION("if not exists") {
        auto result = cql::parse("INSERT INTO tbl (id) VALUES (1) IF NOT EXISTS;");
        REQUIRE(result.has_value());
        auto& stmt = get<Insert>(result->value);
        REQUIRE(stmt.if_not_exists == true);
    }
}

TEST_CASE("CQL SELECT", "[objstore.cql]") {
    SECTION("select star") {
        auto result = cql::parse("SELECT * FROM tbl;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.from.table_name == "tbl");
    }

    SECTION("INSERT INTO with string values") {
        auto query = "INSERT INTO my_ks.table VALUES ('text1', 'text2');";
        auto result = cql::parse(query);
    }

    SECTION("select columns") {
        auto result = cql::parse("SELECT id, name FROM ks.tbl;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.from.table_name == "tbl");
    }

    SECTION("INSERT INTO with mixed values") {
        auto query = "INSERT INTO app.users VALUES (123, 'John Doe', 'john@example.com');";
        auto result = cql::parse(query);
    }

    SECTION("select with where") {
        auto result = cql::parse("SELECT * FROM tbl WHERE id = 1;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.where.has_value());
    }

    SECTION("INSERT INTO with single value") {
        auto query = "INSERT INTO test.data VALUES (42);";
        auto result = cql::parse(query);
    }

    SECTION("select with limit") {
        auto result = cql::parse("SELECT * FROM tbl LIMIT 10;");
        REQUIRE(result.has_value());
        [[maybe_unused]] auto& stmt = get<Select>(result->value);
    }

    SECTION("INSERT INTO with negative integers") {
        // @note negation is represented as UnaryMinusArithmeticOperation, not a folded Constant
        auto query = "INSERT INTO ks.data VALUES (-100, -50, -1);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        const auto& nv = get<Insert::NamesValues>(ins.insert_clause);
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

    SECTION("INSERT INTO with large integer") {
        auto query = "INSERT INTO ks.data VALUES (9223372036854775807);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 1);
        REQUIRE(get<S64>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == 9223372036854775807LL);
    }

    SECTION("INSERT INTO case insensitive") {
        auto query = "insert into ks.tbl values (1, 'test');";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
    }

    SECTION("INSERT INTO with empty string") {
        auto query = "INSERT INTO ks.tbl VALUES ('');";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 1);
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == "");
    }

    SECTION("INSERT INTO with string containing spaces") {
        auto query = "INSERT INTO ks.tbl VALUES ('hello world', 'foo bar baz');";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 2);
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == "hello world");
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[1].value).value) == "foo bar baz");
    }

    SECTION("INSERT INTO with escaped quotes") {
        // @note CQL uses '' to escape single quotes inside strings, not backslash
        auto query = "INSERT INTO ks.tbl VALUES ('''quoted''');";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 1);
        REQUIRE(get<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == "'quoted'");
    }

    SECTION("INSERT INTO with zero value") {
        auto query = "INSERT INTO ks.tbl VALUES (0);";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<S64>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value) == 0);
    }

    SECTION("INSERT INTO with multiple string values") {
        auto query = "INSERT INTO app.messages VALUES ('msg1', 'msg2', 'msg3', 'msg4', 'msg5');";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& ins = get<Insert>(result->value);
        REQUIRE(get<Insert::NamesValues>(ins.insert_clause).values.length == 5);
        for (size_t i = 0; i < 5; ++i) {
            REQUIRE(type_matches_tag<AutoString8>(get<Constant>(get<Insert::NamesValues>(ins.insert_clause).values[0].value).value));
        }
    }
}

TEST_CASE("CQL SELECT FROM statements", "[objstore.parser]") {
    SECTION("Basic SELECT FROM") {
        auto query = "SELECT * FROM ks.users;";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));

        const auto& sel = get<Select>(result->value);
        REQUIRE(*sel.from.keyspace_name == "ks");
        REQUIRE(sel.from.table_name == "users");
    }

    SECTION("SELECT FROM case insensitive") {
        auto query = "select * from TestTable;";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));

        const auto& sel = get<Select>(result->value);
        REQUIRE(!sel.from.keyspace_name);
        REQUIRE(sel.from.table_name == "testtable"); // @note CQL folds unquoted identifiers to lowercase
    }

    SECTION("SELECT FROM with underscores") {
        auto query = "SELECT * FROM my_table;";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& sel = get<Select>(result->value);
        REQUIRE(!sel.from.keyspace_name);
        REQUIRE(sel.from.table_name == "my_table");
    }

    SECTION("SELECT FROM with mixed case keywords") {
        auto query = "SeLeCt * FrOm ks.tbl;";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
    }

    SECTION("SELECT FROM with extra whitespace") {
        auto query = "SELECT   *   FROM   ks.users  ;";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& sel = get<Select>(result->value);
        REQUIRE(*sel.from.keyspace_name == "ks");
        REQUIRE(sel.from.table_name == "users");
    }

    SECTION("SELECT FROM with leading/trailing whitespace") {
        auto query = "  SELECT * FROM ks.users;  ";
        auto result = cql::parse(query);

        REQUIRE(result.has_value());
        const auto& sel = get<Select>(result->value);
        REQUIRE(*sel.from.keyspace_name == "ks");
        REQUIRE(sel.from.table_name == "users");
    }
}

TEST_CASE("CQL Invalid syntax handling", "[objstore.parser]") {
    SECTION("Invalid keyword") {
        auto query = "INVALID STATEMENT;";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing table name in CREATE TABLE") {
        auto query = "CREATE TABLE;";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("SELECT with column selection") {
        // @todo
    }

    SECTION("Missing parentheses in CREATE TABLE") {
        auto query = "CREATE TABLE ks.users id int PRIMARY KEY;";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing WITH in CREATE KEYSPACE") {
        auto query = "CREATE KEYSPACE ks replication = 'test';";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Empty query") {
        auto query = "";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Only whitespace") {
        auto query = "   \n\t  ";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Unclosed string in INSERT") {
        auto query = "INSERT INTO ks.tbl VALUES ('unclosed);";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing comma between columns") {
        auto query = "CREATE TABLE ks.test (id int PRIMARY KEY name text);";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing comma between values") {
        auto query = "INSERT INTO ks.tbl VALUES (1 2 3);";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Invalid data type") {
        auto query = "CREATE TABLE ks.test (id invalidtype PRIMARY KEY);";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing closing parenthesis in INSERT") {
        auto query = "INSERT INTO ks.tbl VALUES (1, 2, 3;";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Missing closing parenthesis in CREATE TABLE") {
        auto query = "CREATE TABLE ks.test (id int PRIMARY KEY;";
        auto result = cql::parse(query);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("select json") {
        auto result = cql::parse("SELECT JSON * FROM tbl;");
        REQUIRE(result.has_value());
        auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.transform.has_value());
    }
}

TEST_CASE("CQL UPDATE", "[objstore.cql]") {
    auto result = cql::parse("UPDATE tbl SET name = 'new' WHERE id = 1;");
    REQUIRE(result.has_value());
    auto& stmt = get<Update>(result->value);
    REQUIRE(stmt.table.table_name == "tbl");
    REQUIRE(stmt.assignments.length == 1);
}

TEST_CASE("CQL DELETE", "[objstore.cql]") {
    auto result = cql::parse("DELETE FROM tbl WHERE id = 1;");
    REQUIRE(result.has_value());
    auto& stmt = get<Delete>(result->value);
    REQUIRE(stmt.table.table_name == "tbl");
}

TEST_CASE("CQL case insensitive keywords", "[objstore.cql]") {
    REQUIRE(cql::parse("select * from tbl;").has_value());
    REQUIRE(cql::parse("Select * From tbl;").has_value());
    REQUIRE(cql::parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'};").has_value());
}

TEST_CASE("Parse USE statement", "[objstore.parser]") {
    SECTION("Basic USE") {
        auto result = cql::parse("USE my_keyspace;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<UseKeyspace>(result->value));
        const auto& stmt = get<UseKeyspace>(result->value);
        REQUIRE(stmt.keyspace == "my_keyspace");
    }
}

TEST_CASE("Parse DROP statements", "[objstore.parser]") {
    SECTION("DROP KEYSPACE") {
        auto result = cql::parse("DROP KEYSPACE my_keyspace;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropKeyspace>(result->value));
        const auto& stmt = get<DropKeyspace>(result->value);
        REQUIRE(stmt.keyspace == "my_keyspace");
        REQUIRE_FALSE(stmt.if_exists);
    }

    SECTION("DROP KEYSPACE IF EXISTS") {
        auto result = cql::parse("DROP KEYSPACE IF EXISTS my_keyspace;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropKeyspace>(result->value));
        const auto& stmt = get<DropKeyspace>(result->value);
        REQUIRE(stmt.keyspace == "my_keyspace");
        REQUIRE(stmt.if_exists);
    }

    SECTION("DROP TABLE") {
        auto result = cql::parse("DROP TABLE ks.my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropTable>(result->value));
        const auto& stmt = get<DropTable>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "my_table");
        REQUIRE_FALSE(stmt.if_exists);
    }

    SECTION("DROP TABLE IF EXISTS") {
        auto result = cql::parse("DROP TABLE IF EXISTS my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<DropTable>(result->value));
        const auto& stmt = get<DropTable>(result->value);
        REQUIRE(!stmt.table.keyspace_name);
        REQUIRE(stmt.table.table_name == "my_table");
        REQUIRE(stmt.if_exists);
    }
}

TEST_CASE("Parse TRUNCATE statement", "[objstore.parser]") {
    SECTION("TRUNCATE with keyspace") {
        auto result = cql::parse("TRUNCATE ks.my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<TruncateTable>(result->value));
        const auto& stmt = get<TruncateTable>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "my_table");
    }

    SECTION("TRUNCATE TABLE with keyspace") {
        auto result = cql::parse("TRUNCATE TABLE my_table;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<TruncateTable>(result->value));
        const auto& stmt = get<TruncateTable>(result->value);
        REQUIRE(!stmt.table.keyspace_name);
        REQUIRE(stmt.table.table_name == "my_table");
    }
}

TEST_CASE("Parse UPDATE statement", "[objstore.parser]") {
    SECTION("Basic UPDATE") {
        auto result = cql::parse("UPDATE ks.users SET name = 'Alice' WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Update>(result->value));
        const auto& stmt = get<Update>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("UPDATE multiple assignments") {
        auto result = cql::parse("UPDATE ks.users SET name = 'Alice', age = 30 WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Update>(result->value));
        [[maybe_unused]] const auto& stmt = get<Update>(result->value);
        // @todo
    }
}

TEST_CASE("Parse DELETE statement", "[objstore.parser]") {
    SECTION("DELETE all columns") {
        auto result = cql::parse("DELETE FROM ks.users WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Delete>(result->value));
        const auto& stmt = get<Delete>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("DELETE specific columns") {
        auto result = cql::parse("DELETE name, age FROM users WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Delete>(result->value));
        [[maybe_unused]] const auto& stmt = get<Delete>(result->value);
        // @todo
    }
}

TEST_CASE("Parse SELECT with WHERE and LIMIT", "[objstore.parser]") {
    SECTION("SELECT with WHERE") {
        auto result = cql::parse("SELECT * FROM ks.users WHERE id = 1;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(*stmt.from.keyspace_name == "ks");
        REQUIRE(stmt.from.table_name == "users");
        // @todo
    }

    SECTION("SELECT with LIMIT") {
        auto result = cql::parse("SELECT * FROM ks.users LIMIT 10;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(get<S64>(stmt.limit.value) == 10);
    }

    SECTION("SELECT with WHERE and LIMIT") {
        auto result = cql::parse("SELECT * FROM users WHERE active = true LIMIT 5;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(!stmt.from.keyspace_name);
        REQUIRE(stmt.from.table_name == "users");
        REQUIRE(get<S64>(stmt.limit.value) == 5);
        // @todo
    }

    SECTION("SELECT specific columns") {
        auto result = cql::parse("SELECT id, name, age FROM ks.users;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("SELECT multiple WHERE conditions") {
        auto result = cql::parse("SELECT * FROM ks.users WHERE id = 1 AND name = 'Alice';");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }
}

TEST_CASE("Parse INSERT with column names", "[objstore.parser]") {
    SECTION("INSERT with column list") {
        auto result = cql::parse("INSERT INTO ks.users (id, name) VALUES (1, 'Alice');");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
        const auto& stmt = get<Insert>(result->value);
        REQUIRE(*stmt.table.keyspace_name == "ks");
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("INSERT without column list") {
        auto result = cql::parse("INSERT INTO users VALUES (1, 'Alice', 30);");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
        const auto& stmt = get<Insert>(result->value);
        REQUIRE(!stmt.table.keyspace_name);
        REQUIRE(stmt.table.table_name == "users");
        // @todo
    }

    SECTION("INSERT IF NOT EXISTS") {
        auto result = cql::parse("INSERT INTO ks.users (id) VALUES (1) IF NOT EXISTS;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Insert>(result->value));
        const auto& stmt = get<Insert>(result->value);
        REQUIRE(stmt.if_not_exists);
    }
}

TEST_CASE("Parse SELECT with ORDER BY", "[objstore.parser]") {
    SECTION("ORDER BY single column ascending") {
        auto result = cql::parse("SELECT * FROM ks.users ORDER BY created_at ASC;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("ORDER BY single column descending") {
        auto result = cql::parse("SELECT * FROM ks.users ORDER BY created_at DESC;");
        REQUIRE(result.has_value());
        const auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.order_by->columns.length == 1);
        REQUIRE(stmt.order_by->columns[0].sort == Sort::DESC);
    }

    SECTION("ORDER BY default ascending") {
        auto result = cql::parse("SELECT * FROM ks.users ORDER BY name;");
        REQUIRE(result.has_value());
        const auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.order_by->columns.length == 1);
        REQUIRE(stmt.order_by->columns[0].sort == Sort::ASC);
        REQUIRE(stmt.order_by->columns[0].column.identifier == "name");
    }

    SECTION("ORDER BY with WHERE and LIMIT") {
        auto result = cql::parse("SELECT * FROM ks.users WHERE id = 1 ORDER BY created_at DESC LIMIT 10;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }
}

TEST_CASE("Parse SELECT with ALLOW FILTERING", "[objstore.parser]") {
    SECTION("Basic ALLOW FILTERING") {
        auto result = cql::parse("SELECT * FROM ks.users WHERE age = 25 ALLOW FILTERING;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        const auto& stmt = get<Select>(result->value);
        REQUIRE(stmt.allow_filtering == true);
    }

    SECTION("ALLOW FILTERING with ORDER BY and LIMIT") {
        auto result = cql::parse("SELECT * FROM ks.users WHERE age = 25 ORDER BY name LIMIT 100 ALLOW FILTERING;");
        REQUIRE(result.has_value());
        const auto& stmt = get<Select>(result->value);
        REQUIRE(get<S64>(stmt.limit.value) == 100);
        REQUIRE(stmt.allow_filtering == true);
        // @todo
    }
}

TEST_CASE("Parse SELECT with GROUP BY", "[objstore.parser]") {
    SECTION("GROUP BY single column") {
        auto result = cql::parse("SELECT user_id FROM ks.events GROUP BY user_id;");
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<Select>(result->value));
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("GROUP BY multiple columns") {
        auto result = cql::parse("SELECT * FROM ks.events GROUP BY year, month, day;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("GROUP BY with WHERE and ORDER BY") {
        auto result = cql::parse("SELECT * FROM ks.events WHERE user_id = 1 GROUP BY event_type ORDER BY created_at DESC;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }

    SECTION("GROUP BY with LIMIT and ALLOW FILTERING") {
        auto result = cql::parse("SELECT * FROM ks.events GROUP BY user_id LIMIT 50 ALLOW FILTERING;");
        REQUIRE(result.has_value());
        [[maybe_unused]] const auto& stmt = get<Select>(result->value);
        // @todo
    }
}

TEST_CASE("Parse CREATE KEYSPACE with map literal replication", "[objstore.parser]") {
    SECTION("Simple map replication") {
        auto result = cql::parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'};");
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
        auto result = cql::parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 3};");
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        [[maybe_unused]] const auto& map = get<MapLiteral>(ks.options.identifier_values[0].second);
        // @todo
    }

    SECTION("NetworkTopologyStrategy with datacenter configs") {
        auto result = cql::parse("CREATE KEYSPACE ks WITH replication = {'class': 'NetworkTopologyStrategy', 'dc1': 3, 'dc2': 2};");
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        [[maybe_unused]] const auto& map = get<MapLiteral>(ks.options.identifier_values[0].second);
        // @todo
    }

    SECTION("Mix of map and scalar options") {
        auto result = cql::parse("CREATE KEYSPACE ks WITH replication = {'class': 'SimpleStrategy'} AND durable_writes = 'true';");
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspace>(result->value);
        REQUIRE(ks.options.identifier_values.length == 2);
        REQUIRE(type_matches_tag<MapLiteral>(ks.options.identifier_values[0].second));
        // @note quoted option values are stored as Constant{AutoString8}
        REQUIRE(type_matches_tag<Constant>(ks.options.identifier_values[1].second));
        REQUIRE(get<AutoString8>(get<Constant>(ks.options.identifier_values[1].second).value) == "true");
    }
}

TEST_CASE("CQL parse error reporting", "[objstore.parser]") {
    SECTION("Invalid syntax returns empty with Catch2 reporter") {
        auto result = cql::parse("INVALID STATEMENT;");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Valid query succeeds with Catch2 reporter") {
        auto result = cql::parse("SELECT * FROM ks.tbl;");
        REQUIRE(result.has_value());
    }

    SECTION("Empty query returns empty with Catch2 reporter") {
        auto result = cql::parse("");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Unclosed string returns empty with Catch2 reporter") {
        auto result = cql::parse("INSERT INTO ks.tbl VALUES ('unclosed);");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Invalid syntax returns empty with bool reporter") {
        auto result = cql::parse("CREATE TABLE;");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("CQL quoted identifiers", "[objstore.cql]") {
    auto result = cql::parse("SELECT * FROM \"MyTable\";");
    REQUIRE(result.has_value());
    auto& stmt = get<Select>(result->value);
    REQUIRE(stmt.from.table_name == "MyTable");
}

TEST_CASE("CQL string escape", "[objstore.cql]") {
    auto result = cql::parse("INSERT INTO tbl (id, name) VALUES (1, 'it''s');");
    REQUIRE(result.has_value());
    auto& stmt = get<Insert>(result->value);
    auto& nv = get<Insert::NamesValues>(stmt.insert_clause);
    auto& str_val = get<AutoString8>(get<Constant>(nv.values[1].value).value);
    REQUIRE(str_val == "it's");
}
