#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

import plexdb.base;
import plexdb.tagged_union;

import objstore.parser;
import objstore.dtypes;
import objstore.parser.stl_helpers;

using namespace plexdb;
using namespace objstore;
using namespace objstore::parser;

TEST_CASE("CREATE KEYSPACE statements", "[objstore.parser]") {
    SECTION("Basic CREATE KEYSPACE") {
        auto query = "CREATE KEYSPACE my_keyspace WITH replication = 'SimpleStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspaceRequest>(result->value));
        
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.keyspace_name == "my_keyspace");
        REQUIRE(ks.if_not_exists == false);
        REQUIRE(ks.options.length == 1);
        REQUIRE(ks.options[0].key == "replication");
        REQUIRE(type_matches_tag<STLString>(ks.options[0].value));
        REQUIRE(get<STLString>(ks.options[0].value) == "SimpleStrategy");
    }
    
    SECTION("CREATE KEYSPACE IF NOT EXISTS") {
        auto query = "CREATE KEYSPACE IF NOT EXISTS test_ks WITH replication = 'NetworkTopologyStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspaceRequest>(result->value));
        
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.keyspace_name == "test_ks");
        REQUIRE(ks.if_not_exists == true);
        REQUIRE(ks.options.length == 1);
        REQUIRE(ks.options[0].key == "replication");
        REQUIRE(type_matches_tag<STLString>(ks.options[0].value));
        REQUIRE(get<STLString>(ks.options[0].value) == "NetworkTopologyStrategy");
    }
    
    SECTION("CREATE KEYSPACE with multiple options") {
        auto query = "CREATE KEYSPACE prod WITH replication = 'SimpleStrategy' AND durable_writes = 'true';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspaceRequest>(result->value));
        
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.keyspace_name == "prod");
        REQUIRE(ks.if_not_exists == false);
        REQUIRE(ks.options.length == 2);
        REQUIRE(ks.options[0].key == "replication");
        REQUIRE(type_matches_tag<STLString>(ks.options[0].value));
        REQUIRE(get<STLString>(ks.options[0].value) == "SimpleStrategy");
        REQUIRE(ks.options[1].key == "durable_writes");
        REQUIRE(type_matches_tag<STLString>(ks.options[1].value));
        REQUIRE(get<STLString>(ks.options[1].value) == "true");
    }
    
    SECTION("CREATE KEYSPACE with three options") {
        auto query = "CREATE KEYSPACE multi_opt WITH replication = 'NetworkTopologyStrategy' AND durable_writes = 'true' AND strategy_class = 'SimpleStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspaceRequest>(result->value));
        
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.options.length == 3);
        REQUIRE(ks.options[0].key == "replication");
        REQUIRE(ks.options[1].key == "durable_writes");
        REQUIRE(ks.options[2].key == "strategy_class");
    }
    
    SECTION("CREATE KEYSPACE case insensitive") {
        auto query = "create keyspace TestKS with replication = 'test';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspaceRequest>(result->value));
        
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.keyspace_name == "TestKS");
    }
    
    SECTION("CREATE KEYSPACE without semicolon") {
        auto query = "CREATE KEYSPACE no_semi WITH replication = 'test'";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateKeyspaceRequest>(result->value));
        
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.keyspace_name == "no_semi");
    }
    
    SECTION("CREATE KEYSPACE with underscore in name") {
        auto query = "CREATE KEYSPACE my_test_keyspace WITH replication = 'SimpleStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.keyspace_name == "my_test_keyspace");
    }
    
    SECTION("CREATE KEYSPACE with mixed case IF NOT EXISTS") {
        auto query = "CREATE KEYSPACE If Not Exists mixed_case WITH replication = 'test';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.if_not_exists == true);
        REQUIRE(ks.keyspace_name == "mixed_case");
    }
    
    SECTION("CREATE KEYSPACE with quoted option value") {
        auto query = "CREATE KEYSPACE ks WITH replication = '{\\'class\\': \\'SimpleStrategy\\'}'";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ks = get<CreateKeyspaceRequest>(result->value);
        REQUIRE(ks.options.length == 1);
    }
}

TEST_CASE("CREATE TABLE statements", "[objstore.parser]") {
    SECTION("Basic CREATE TABLE with single column") {
        auto query = "CREATE TABLE users (id int PRIMARY KEY);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTableRequest>(result->value));
        
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.table_name == "users");
        REQUIRE(tbl.if_not_exists == false);
        REQUIRE(tbl.columns.length == 1);
        REQUIRE(tbl.columns[0].name == "id");
        REQUIRE(tbl.columns[0].dtype == DType::Int);
        REQUIRE(tbl.columns[0].is_primary_key == true);
    }
    
    SECTION("CREATE TABLE with multiple columns") {
        auto query = "CREATE TABLE users (id int PRIMARY KEY, name text, age int);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTableRequest>(result->value));
        
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.table_name == "users");
        REQUIRE(tbl.columns.length == 3);
        
        REQUIRE(tbl.columns[0].name == "id");
        REQUIRE(tbl.columns[0].dtype == DType::Int);
        REQUIRE(tbl.columns[0].is_primary_key == true);
        
        REQUIRE(tbl.columns[1].name == "name");
        REQUIRE(tbl.columns[1].dtype == DType::Text);
        REQUIRE(tbl.columns[1].is_primary_key == false);
        
        REQUIRE(tbl.columns[2].name == "age");
        REQUIRE(tbl.columns[2].dtype == DType::Int);
        REQUIRE(tbl.columns[2].is_primary_key == false);
    }
    
    SECTION("CREATE TABLE IF NOT EXISTS") {
        auto query = "CREATE TABLE IF NOT EXISTS products (sku bigint PRIMARY KEY, name text, price int);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTableRequest>(result->value));
        
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.table_name == "products");
        REQUIRE(tbl.if_not_exists == true);
        REQUIRE(tbl.columns.length == 3);
    }
    
    // SECTION("CREATE TABLE with various data types") {
    //     auto query = "CREATE TABLE data (id int PRIMARY KEY, name text, count bigint, created timestamp, active boolean);";
    //     auto result = parse_cql(query);
        
    //     REQUIRE(result.has_value());
    //     const auto& tbl = get<CreateTableRequest>(result->value);
    //     REQUIRE(tbl.columns.length == 5);
    //     REQUIRE(tbl.columns[0].dtype == DType::Int);
    //     REQUIRE(tbl.columns[1].dtype == DType::Text);
    //     REQUIRE(tbl.columns[2].dtype == DType::BIGINT);
    //     REQUIRE(tbl.columns[3].dtype == DType::TIMESTAMP);
    //     REQUIRE(tbl.columns[4].dtype == DType::BOOLEAN);
    // }
    
    // SECTION("CREATE TABLE with FLOAT and DOUBLE types") {
    //     auto query = "CREATE TABLE metrics (id int PRIMARY KEY, temperature float, precision_value double);";
    //     auto result = parse_cql(query);
        
    //     REQUIRE(result.has_value());
    //     const auto& tbl = get<CreateTableRequest>(result->value);
    //     REQUIRE(tbl.columns.length == 3);
    //     REQUIRE(tbl.columns[1].dtype == DType::FLOAT);
    //     REQUIRE(tbl.columns[2].dtype == DType::DOUBLE);
    // }
    
    // SECTION("CREATE TABLE with UUID type") {
    //     auto query = "CREATE TABLE sessions (session_id uuid PRIMARY KEY, user_id int);";
    //     auto result = parse_cql(query);
        
    //     REQUIRE(result.has_value());
    //     const auto& tbl = get<CreateTableRequest>(result->value);
    //     REQUIRE(tbl.columns.length == 2);
    //     REQUIRE(tbl.columns[0].dtype == DType::UUID);
    //     REQUIRE(tbl.columns[0].is_primary_key == true);
    // }
    
    SECTION("CREATE TABLE case insensitive") {
        auto query = "create table TestTable (Id INT primary key, Name TEXT);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<CreateTableRequest>(result->value));
        
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.table_name == "TestTable");
    }
    
    SECTION("CREATE TABLE with non-primary key as last column") {
        auto query = "CREATE TABLE test (id int PRIMARY KEY, data text);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.columns[0].is_primary_key == true);
        REQUIRE(tbl.columns[1].is_primary_key == false);
    }
    
    SECTION("CREATE TABLE with primary key in middle") {
        auto query = "CREATE TABLE test (name text, id int PRIMARY KEY, email text);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.columns.length == 3);
        REQUIRE(tbl.columns[0].is_primary_key == false);
        REQUIRE(tbl.columns[1].is_primary_key == true);
        REQUIRE(tbl.columns[2].is_primary_key == false);
    }
    
    SECTION("CREATE TABLE with many columns") {
        auto query = "CREATE TABLE large (c1 int PRIMARY KEY, c2 text, c3 bigint, c4 timestamp, c5 boolean, c6 float, c7 double);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.columns.length == 7);
    }
    
    SECTION("CREATE TABLE with underscore column names") {
        auto query = "CREATE TABLE test (user_id int PRIMARY KEY, first_name text, last_name text);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& tbl = get<CreateTableRequest>(result->value);
        REQUIRE(tbl.columns[0].name == "user_id");
        REQUIRE(tbl.columns[1].name == "first_name");
        REQUIRE(tbl.columns[2].name == "last_name");
    }
}

TEST_CASE("INSERT INTO statements", "[objstore.parser]") {
    SECTION("INSERT INTO with integer values") {
        auto query = "INSERT INTO ks.users VALUES (1, 2, 3);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<InsertIntoRequest>(result->value));
        
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.keyspace_name == "ks");
        REQUIRE(ins.table_name == "users");
        REQUIRE(ins.values.length == 3);
        REQUIRE(ins.values[0].dtype == DType::Int);
        REQUIRE(type_matches_tag<S64>(ins.values[0].value));
        REQUIRE(get<S64>(ins.values[0].value) == 1);
        REQUIRE(ins.values[1].dtype == DType::Int);
        REQUIRE(type_matches_tag<S64>(ins.values[1].value));
        REQUIRE(get<S64>(ins.values[1].value) == 2);
        REQUIRE(ins.values[2].dtype == DType::Int);
        REQUIRE(type_matches_tag<S64>(ins.values[2].value));
        REQUIRE(get<S64>(ins.values[2].value) == 3);
    }
    
    SECTION("INSERT INTO with string values") {
        auto query = "INSERT INTO my_ks.table VALUES ('text1', 'text2');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<InsertIntoRequest>(result->value));
        
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 2);
        REQUIRE(ins.values[0].dtype == DType::Text);
        REQUIRE(type_matches_tag<STLString>(ins.values[0].value));
        REQUIRE(get<STLString>(ins.values[0].value) == "text1");
        REQUIRE(ins.values[1].dtype == DType::Text);
        REQUIRE(type_matches_tag<STLString>(ins.values[1].value));
        REQUIRE(get<STLString>(ins.values[1].value) == "text2");
    }
    
    SECTION("INSERT INTO with mixed values") {
        auto query = "INSERT INTO app.users VALUES (123, 'John Doe', 'john@example.com');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.keyspace_name == "app");
        REQUIRE(ins.table_name == "users");
        REQUIRE(ins.values.length == 3);
        REQUIRE(ins.values[0].dtype == DType::Int);
        REQUIRE(type_matches_tag<S64>(ins.values[0].value));
        REQUIRE(get<S64>(ins.values[0].value) == 123);
        REQUIRE(ins.values[1].dtype == DType::Text);
        REQUIRE(type_matches_tag<STLString>(ins.values[1].value));
        REQUIRE(get<STLString>(ins.values[1].value) == "John Doe");
        REQUIRE(ins.values[2].dtype == DType::Text);
        REQUIRE(type_matches_tag<STLString>(ins.values[2].value));
        REQUIRE(get<STLString>(ins.values[2].value) == "john@example.com");
    }
    
    SECTION("INSERT INTO with single value") {
        auto query = "INSERT INTO test.data VALUES (42);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 1);
        REQUIRE(ins.values[0].dtype == DType::Int);
        REQUIRE(type_matches_tag<S64>(ins.values[0].value));
        REQUIRE(get<S64>(ins.values[0].value) == 42);
    }
    
    SECTION("INSERT INTO with negative integers") {
        auto query = "INSERT INTO ks.data VALUES (-100, -50, -1);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 3);
        REQUIRE(get<S64>(ins.values[0].value) == -100);
        REQUIRE(get<S64>(ins.values[1].value) == -50);
        REQUIRE(get<S64>(ins.values[2].value) == -1);
    }
    
    SECTION("INSERT INTO with large integer") {
        auto query = "INSERT INTO ks.data VALUES (9223372036854775807);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 1);
        REQUIRE(get<S64>(ins.values[0].value) == 9223372036854775807LL);
    }
    
    SECTION("INSERT INTO case insensitive") {
        auto query = "insert into ks.tbl values (1, 'test');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<InsertIntoRequest>(result->value));
    }
    
    SECTION("INSERT INTO with empty string") {
        auto query = "INSERT INTO ks.tbl VALUES ('');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 1);
        REQUIRE(get<STLString>(ins.values[0].value) == "");
    }
    
    SECTION("INSERT INTO with string containing spaces") {
        auto query = "INSERT INTO ks.tbl VALUES ('hello world', 'foo bar baz');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 2);
        REQUIRE(get<STLString>(ins.values[0].value) == "hello world");
        REQUIRE(get<STLString>(ins.values[1].value) == "foo bar baz");
    }
    
    SECTION("INSERT INTO with escaped quotes") {
        auto query = "INSERT INTO ks.tbl VALUES ('\\'quoted\\'');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 1);
        REQUIRE(get<STLString>(ins.values[0].value) == "'quoted'");
    }
    
    SECTION("INSERT INTO with zero value") {
        auto query = "INSERT INTO ks.tbl VALUES (0);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(get<S64>(ins.values[0].value) == 0);
    }
    
    SECTION("INSERT INTO with multiple string values") {
        auto query = "INSERT INTO app.messages VALUES ('msg1', 'msg2', 'msg3', 'msg4', 'msg5');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& ins = get<InsertIntoRequest>(result->value);
        REQUIRE(ins.values.length == 5);
        for (size_t i = 0; i < 5; ++i) {
            REQUIRE(ins.values[i].dtype == DType::Text);
        }
    }
}

TEST_CASE("SELECT FROM statements", "[objstore.parser]") {
    SECTION("Basic SELECT FROM") {
        auto query = "SELECT * FROM ks.users;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<SelectFromRequest>(result->value));
        
        const auto& sel = get<SelectFromRequest>(result->value);
        REQUIRE(sel.keyspace_name == "ks");
        REQUIRE(sel.table_name == "users");
    }
    
    SECTION("SELECT FROM without semicolon") {
        auto query = "SELECT * FROM my_app.products";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<SelectFromRequest>(result->value));
        
        const auto& sel = get<SelectFromRequest>(result->value);
        REQUIRE(sel.keyspace_name == "my_app");
        REQUIRE(sel.table_name == "products");
    }
    
    SECTION("SELECT FROM case insensitive") {
        auto query = "select * from TestKS.TestTable;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<SelectFromRequest>(result->value));
        
        const auto& sel = get<SelectFromRequest>(result->value);
        REQUIRE(sel.keyspace_name == "TestKS");
        REQUIRE(sel.table_name == "TestTable");
    }
    
    SECTION("SELECT FROM with underscores") {
        auto query = "SELECT * FROM my_keyspace.my_table;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& sel = get<SelectFromRequest>(result->value);
        REQUIRE(sel.keyspace_name == "my_keyspace");
        REQUIRE(sel.table_name == "my_table");
    }
    
    SECTION("SELECT FROM with mixed case keywords") {
        auto query = "SeLeCt * FrOm ks.tbl;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(type_matches_tag<SelectFromRequest>(result->value));
    }
    
    SECTION("SELECT FROM with extra whitespace") {
        auto query = "SELECT   *   FROM   ks.users  ;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& sel = get<SelectFromRequest>(result->value);
        REQUIRE(sel.keyspace_name == "ks");
        REQUIRE(sel.table_name == "users");
    }
    
    SECTION("SELECT FROM with leading/trailing whitespace") {
        auto query = "  SELECT * FROM ks.users;  ";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        const auto& sel = get<SelectFromRequest>(result->value);
        REQUIRE(sel.keyspace_name == "ks");
        REQUIRE(sel.table_name == "users");
    }
}

TEST_CASE("Invalid syntax handling", "[objstore.parser]") {
    SECTION("Invalid keyword") {
        auto query = "INVALID STATEMENT;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing table name in CREATE TABLE") {
        auto query = "CREATE TABLE;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing VALUES keyword in INSERT") {
        auto query = "INSERT INTO ks.table (1, 2);";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing keyspace in INSERT") {
        auto query = "INSERT INTO table VALUES (1);";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing keyspace in SELECT") {
        auto query = "SELECT * FROM table;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Wrong SELECT syntax - column selection") {
        auto query = "SELECT id FROM ks.table;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing parentheses in CREATE TABLE") {
        auto query = "CREATE TABLE users id int PRIMARY KEY;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing WITH in CREATE KEYSPACE") {
        auto query = "CREATE KEYSPACE ks replication = 'test';";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Empty query") {
        auto query = "";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Only whitespace") {
        auto query = "   \n\t  ";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Unclosed string in INSERT") {
        auto query = "INSERT INTO ks.tbl VALUES ('unclosed);";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing comma between columns") {
        auto query = "CREATE TABLE test (id int PRIMARY KEY name text);";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing comma between values") {
        auto query = "INSERT INTO ks.tbl VALUES (1 2 3);";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Invalid data type") {
        auto query = "CREATE TABLE test (id invalidtype PRIMARY KEY);";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing closing parenthesis in INSERT") {
        auto query = "INSERT INTO ks.tbl VALUES (1, 2, 3;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
    
    SECTION("Missing closing parenthesis in CREATE TABLE") {
        auto query = "CREATE TABLE test (id int PRIMARY KEY;";
        auto result = parse_cql(query);
        REQUIRE_FALSE(result.has_value());
    }
}