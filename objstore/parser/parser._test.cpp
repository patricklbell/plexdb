#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

import objstore.parser;

using namespace objstore::parser;

using Catch::Matchers::Equals;

TEST_CASE("CREATE KEYSPACE statements", "[objstore.parser]") {
    SECTION("Basic CREATE KEYSPACE") {
        auto query = "CREATE KEYSPACE my_keyspace WITH replication = 'SimpleStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_KEYSPACE);
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("my_keyspace"));
        REQUIRE(result->create_keyspace.if_not_exists == false);
        REQUIRE(result->create_keyspace.options.size() == 1);
        REQUIRE_THAT(result->create_keyspace.options[0].key, Equals("replication"));
        REQUIRE_THAT(result->create_keyspace.options[0].value, Equals("SimpleStrategy"));
    }
    
    SECTION("CREATE KEYSPACE IF NOT EXISTS") {
        auto query = "CREATE KEYSPACE IF NOT EXISTS test_ks WITH replication = 'NetworkTopologyStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_KEYSPACE);
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("test_ks"));
        REQUIRE(result->create_keyspace.if_not_exists == true);
        REQUIRE(result->create_keyspace.options.size() == 1);
        REQUIRE_THAT(result->create_keyspace.options[0].key, Equals("replication"));
        REQUIRE_THAT(result->create_keyspace.options[0].value, Equals("NetworkTopologyStrategy"));
    }
    
    SECTION("CREATE KEYSPACE with multiple options") {
        auto query = "CREATE KEYSPACE prod WITH replication = 'SimpleStrategy' AND durable_writes = 'true';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_KEYSPACE);
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("prod"));
        REQUIRE(result->create_keyspace.if_not_exists == false);
        REQUIRE(result->create_keyspace.options.size() == 2);
        REQUIRE_THAT(result->create_keyspace.options[0].key, Equals("replication"));
        REQUIRE_THAT(result->create_keyspace.options[0].value, Equals("SimpleStrategy"));
        REQUIRE_THAT(result->create_keyspace.options[1].key, Equals("durable_writes"));
        REQUIRE_THAT(result->create_keyspace.options[1].value, Equals("true"));
    }
    
    SECTION("CREATE KEYSPACE with three options") {
        auto query = "CREATE KEYSPACE multi_opt WITH replication = 'NetworkTopologyStrategy' AND durable_writes = 'true' AND strategy_class = 'SimpleStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_KEYSPACE);
        REQUIRE(result->create_keyspace.options.size() == 3);
        REQUIRE_THAT(result->create_keyspace.options[0].key, Equals("replication"));
        REQUIRE_THAT(result->create_keyspace.options[1].key, Equals("durable_writes"));
        REQUIRE_THAT(result->create_keyspace.options[2].key, Equals("strategy_class"));
    }
    
    SECTION("CREATE KEYSPACE case insensitive") {
        auto query = "create keyspace TestKS with replication = 'test';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_KEYSPACE);
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("TestKS"));
    }
    
    SECTION("CREATE KEYSPACE without semicolon") {
        auto query = "CREATE KEYSPACE no_semi WITH replication = 'test'";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_KEYSPACE);
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("no_semi"));
    }
    
    SECTION("CREATE KEYSPACE with underscore in name") {
        auto query = "CREATE KEYSPACE my_test_keyspace WITH replication = 'SimpleStrategy';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("my_test_keyspace"));
    }
    
    SECTION("CREATE KEYSPACE with mixed case IF NOT EXISTS") {
        auto query = "CREATE KEYSPACE If Not Exists mixed_case WITH replication = 'test';";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_keyspace.if_not_exists == true);
        REQUIRE_THAT(result->create_keyspace.keyspace_name, Equals("mixed_case"));
    }
    
    SECTION("CREATE KEYSPACE with quoted option value") {
        auto query = "CREATE KEYSPACE ks WITH replication = '{\\'class\\': \\'SimpleStrategy\\'}'";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_keyspace.options.size() == 1);
    }
}

TEST_CASE("CREATE TABLE statements", "[objstore.parser]") {
    SECTION("Basic CREATE TABLE with single column") {
        auto query = "CREATE TABLE users (id int PRIMARY KEY);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_TABLE);
        REQUIRE_THAT(result->create_table.table_name, Equals("users"));
        REQUIRE(result->create_table.if_not_exists == false);
        REQUIRE(result->create_table.columns.size() == 1);
        REQUIRE_THAT(result->create_table.columns[0].name, Equals("id"));
        REQUIRE(result->create_table.columns[0].dtype == DType::INT);
        REQUIRE(result->create_table.columns[0].is_primary_key == true);
    }
    
    SECTION("CREATE TABLE with multiple columns") {
        auto query = "CREATE TABLE users (id int PRIMARY KEY, name text, age int);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_TABLE);
        REQUIRE_THAT(result->create_table.table_name, Equals("users"));
        REQUIRE(result->create_table.columns.size() == 3);
        
        REQUIRE_THAT(result->create_table.columns[0].name, Equals("id"));
        REQUIRE(result->create_table.columns[0].dtype == DType::INT);
        REQUIRE(result->create_table.columns[0].is_primary_key == true);
        
        REQUIRE_THAT(result->create_table.columns[1].name, Equals("name"));
        REQUIRE(result->create_table.columns[1].dtype == DType::TEXT);
        REQUIRE(result->create_table.columns[1].is_primary_key == false);
        
        REQUIRE_THAT(result->create_table.columns[2].name, Equals("age"));
        REQUIRE(result->create_table.columns[2].dtype == DType::INT);
        REQUIRE(result->create_table.columns[2].is_primary_key == false);
    }
    
    SECTION("CREATE TABLE IF NOT EXISTS") {
        auto query = "CREATE TABLE IF NOT EXISTS products (sku bigint PRIMARY KEY, name text, price int);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_TABLE);
        REQUIRE_THAT(result->create_table.table_name, Equals("products"));
        REQUIRE(result->create_table.if_not_exists == true);
        REQUIRE(result->create_table.columns.size() == 3);
    }
    
    SECTION("CREATE TABLE with various data types") {
        auto query = "CREATE TABLE data (id int PRIMARY KEY, name text, count bigint, created timestamp, active boolean);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_table.columns.size() == 5);
        REQUIRE(result->create_table.columns[0].dtype == DType::INT);
        REQUIRE(result->create_table.columns[1].dtype == DType::TEXT);
        REQUIRE(result->create_table.columns[2].dtype == DType::BIGINT);
        REQUIRE(result->create_table.columns[3].dtype == DType::TIMESTAMP);
        REQUIRE(result->create_table.columns[4].dtype == DType::BOOLEAN);
    }
    
    SECTION("CREATE TABLE with FLOAT and DOUBLE types") {
        auto query = "CREATE TABLE metrics (id int PRIMARY KEY, temperature float, precision_value double);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_table.columns.size() == 3);
        REQUIRE(result->create_table.columns[1].dtype == DType::FLOAT);
        REQUIRE(result->create_table.columns[2].dtype == DType::DOUBLE);
    }
    
    SECTION("CREATE TABLE with UUID type") {
        auto query = "CREATE TABLE sessions (session_id uuid PRIMARY KEY, user_id int);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_table.columns.size() == 2);
        REQUIRE(result->create_table.columns[0].dtype == DType::UUID);
        REQUIRE(result->create_table.columns[0].is_primary_key == true);
    }
    
    SECTION("CREATE TABLE case insensitive") {
        auto query = "create table TestTable (Id INT primary key, Name TEXT);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::CREATE_TABLE);
        REQUIRE_THAT(result->create_table.table_name, Equals("TestTable"));
    }
    
    SECTION("CREATE TABLE with non-primary key as last column") {
        auto query = "CREATE TABLE test (id int PRIMARY KEY, data text);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_table.columns[0].is_primary_key == true);
        REQUIRE(result->create_table.columns[1].is_primary_key == false);
    }
    
    SECTION("CREATE TABLE with primary key in middle") {
        auto query = "CREATE TABLE test (name text, id int PRIMARY KEY, email text);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_table.columns.size() == 3);
        REQUIRE(result->create_table.columns[0].is_primary_key == false);
        REQUIRE(result->create_table.columns[1].is_primary_key == true);
        REQUIRE(result->create_table.columns[2].is_primary_key == false);
    }
    
    SECTION("CREATE TABLE with many columns") {
        auto query = "CREATE TABLE large (c1 int PRIMARY KEY, c2 text, c3 bigint, c4 timestamp, c5 boolean, c6 float, c7 double);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->create_table.columns.size() == 7);
    }
    
    SECTION("CREATE TABLE with underscore column names") {
        auto query = "CREATE TABLE test (user_id int PRIMARY KEY, first_name text, last_name text);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE_THAT(result->create_table.columns[0].name, Equals("user_id"));
        REQUIRE_THAT(result->create_table.columns[1].name, Equals("first_name"));
        REQUIRE_THAT(result->create_table.columns[2].name, Equals("last_name"));
    }
}

TEST_CASE("INSERT INTO statements", "[objstore.parser]") {
    SECTION("INSERT INTO with integer values") {
        auto query = "INSERT INTO ks.users VALUES (1, 2, 3);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::INSERT_INTO);
        REQUIRE_THAT(result->insert_into.keyspace_name, Equals("ks"));
        REQUIRE_THAT(result->insert_into.table_name, Equals("users"));
        REQUIRE(result->insert_into.values.size() == 3);
        REQUIRE(result->insert_into.values[0].dtype == DType::INT);
        REQUIRE(result->insert_into.values[0]._int == 1);
        REQUIRE(result->insert_into.values[1].dtype == DType::INT);
        REQUIRE(result->insert_into.values[1]._int == 2);
        REQUIRE(result->insert_into.values[2].dtype == DType::INT);
        REQUIRE(result->insert_into.values[2]._int == 3);
    }
    
    SECTION("INSERT INTO with string values") {
        auto query = "INSERT INTO my_ks.table VALUES ('text1', 'text2');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::INSERT_INTO);
        REQUIRE(result->insert_into.values.size() == 2);
        REQUIRE(result->insert_into.values[0].dtype == DType::TEXT);
        REQUIRE_THAT(result->insert_into.values[0].text, Equals("text1"));
        REQUIRE(result->insert_into.values[1].dtype == DType::TEXT);
        REQUIRE_THAT(result->insert_into.values[1].text, Equals("text2"));
    }
    
    SECTION("INSERT INTO with mixed values") {
        auto query = "INSERT INTO app.users VALUES (123, 'John Doe', 'john@example.com');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE_THAT(result->insert_into.keyspace_name, Equals("app"));
        REQUIRE_THAT(result->insert_into.table_name, Equals("users"));
        REQUIRE(result->insert_into.values.size() == 3);
        REQUIRE(result->insert_into.values[0].dtype == DType::INT);
        REQUIRE(result->insert_into.values[0]._int == 123);
        REQUIRE(result->insert_into.values[1].dtype == DType::TEXT);
        REQUIRE_THAT(result->insert_into.values[1].text, Equals("John Doe"));
        REQUIRE(result->insert_into.values[2].dtype == DType::TEXT);
        REQUIRE_THAT(result->insert_into.values[2].text, Equals("john@example.com"));
    }
    
    SECTION("INSERT INTO with single value") {
        auto query = "INSERT INTO test.data VALUES (42);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 1);
        REQUIRE(result->insert_into.values[0].dtype == DType::INT);
        REQUIRE(result->insert_into.values[0]._int == 42);
    }
    
    SECTION("INSERT INTO with negative integers") {
        auto query = "INSERT INTO ks.data VALUES (-100, -50, -1);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 3);
        REQUIRE(result->insert_into.values[0]._int == -100);
        REQUIRE(result->insert_into.values[1]._int == -50);
        REQUIRE(result->insert_into.values[2]._int == -1);
    }
    
    SECTION("INSERT INTO with large integer") {
        auto query = "INSERT INTO ks.data VALUES (9223372036854775807);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 1);
        REQUIRE(result->insert_into.values[0]._int == 9223372036854775807LL);
    }
    
    SECTION("INSERT INTO case insensitive") {
        auto query = "insert into ks.tbl values (1, 'test');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::INSERT_INTO);
    }
    
    SECTION("INSERT INTO with empty string") {
        auto query = "INSERT INTO ks.tbl VALUES ('');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 1);
        REQUIRE_THAT(result->insert_into.values[0].text, Equals(""));
    }
    
    SECTION("INSERT INTO with string containing spaces") {
        auto query = "INSERT INTO ks.tbl VALUES ('hello world', 'foo bar baz');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 2);
        REQUIRE_THAT(result->insert_into.values[0].text, Equals("hello world"));
        REQUIRE_THAT(result->insert_into.values[1].text, Equals("foo bar baz"));
    }
    
    SECTION("INSERT INTO with escaped quotes") {
        auto query = "INSERT INTO ks.tbl VALUES ('\\'quoted\\'');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 1);
        REQUIRE_THAT(result->insert_into.values[0].text, Equals("'quoted'"));
    }
    
    SECTION("INSERT INTO with zero value") {
        auto query = "INSERT INTO ks.tbl VALUES (0);";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values[0]._int == 0);
    }
    
    SECTION("INSERT INTO with multiple string values") {
        auto query = "INSERT INTO app.messages VALUES ('msg1', 'msg2', 'msg3', 'msg4', 'msg5');";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->insert_into.values.size() == 5);
        for (size_t i = 0; i < 5; ++i) {
            REQUIRE(result->insert_into.values[i].dtype == DType::TEXT);
        }
    }
}

TEST_CASE("SELECT FROM statements", "[objstore.parser]") {
    SECTION("Basic SELECT FROM") {
        auto query = "SELECT * FROM ks.users;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::SELECT_FROM);
        REQUIRE_THAT(result->select_from.keyspace_name, Equals("ks"));
        REQUIRE_THAT(result->select_from.table_name, Equals("users"));
    }
    
    SECTION("SELECT FROM without semicolon") {
        auto query = "SELECT * FROM my_app.products";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::SELECT_FROM);
        REQUIRE_THAT(result->select_from.keyspace_name, Equals("my_app"));
        REQUIRE_THAT(result->select_from.table_name, Equals("products"));
    }
    
    SECTION("SELECT FROM case insensitive") {
        auto query = "select * from TestKS.TestTable;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::SELECT_FROM);
        REQUIRE_THAT(result->select_from.keyspace_name, Equals("TestKS"));
        REQUIRE_THAT(result->select_from.table_name, Equals("TestTable"));
    }
    
    SECTION("SELECT FROM with underscores") {
        auto query = "SELECT * FROM my_keyspace.my_table;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE_THAT(result->select_from.keyspace_name, Equals("my_keyspace"));
        REQUIRE_THAT(result->select_from.table_name, Equals("my_table"));
    }
    
    SECTION("SELECT FROM with mixed case keywords") {
        auto query = "SeLeCt * FrOm ks.tbl;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE(result->query_type == CqlQueryType::SELECT_FROM);
    }
    
    SECTION("SELECT FROM with extra whitespace") {
        auto query = "SELECT   *   FROM   ks.users  ;";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE_THAT(result->select_from.keyspace_name, Equals("ks"));
        REQUIRE_THAT(result->select_from.table_name, Equals("users"));
    }
    
    SECTION("SELECT FROM with leading/trailing whitespace") {
        auto query = "  SELECT * FROM ks.users;  ";
        auto result = parse_cql(query);
        
        REQUIRE(result.has_value());
        REQUIRE_THAT(result->select_from.keyspace_name, Equals("ks"));
        REQUIRE_THAT(result->select_from.table_name, Equals("users"));
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