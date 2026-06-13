cmake_minimum_required(VERSION 3.28)

add_library(cql_test_macros INTERFACE)
add_library(cql::test_macros ALIAS cql_test_macros)
target_include_directories(cql_test_macros
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
