cmake_minimum_required(VERSION 3.28)

add_library(plexdb_core_test_macros INTERFACE)
add_library(plexdb::core::test_macros ALIAS plexdb_core_test_macros)
target_include_directories(plexdb_core_test_macros
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
