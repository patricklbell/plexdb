cmake_minimum_required(VERSION 3.28)

project(plexdb_log_plugin)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../core/plugin_abi plugin_abi)

add_library(plexdb_log_plugin SHARED)
target_compile_features(plexdb_log_plugin PRIVATE cxx_std_23)

target_sources(plexdb_log_plugin
    PRIVATE
        log_plugin.cpp
)

target_link_libraries(plexdb_log_plugin
    PRIVATE
        plexdb::core::plugin_abi
)
