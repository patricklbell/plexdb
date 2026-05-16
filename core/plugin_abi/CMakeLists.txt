cmake_minimum_required(VERSION 3.28)

add_library(plexdb_core_plugin_abi INTERFACE)
add_library(plexdb::core::plugin_abi ALIAS plexdb_core_plugin_abi)
target_include_directories(plexdb_core_plugin_abi
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
