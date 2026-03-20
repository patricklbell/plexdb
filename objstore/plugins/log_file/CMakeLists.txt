cmake_minimum_required(VERSION 3.28)
project(objstore_log_file)

add_library(objstore_log_file SHARED)
target_compile_features(objstore_log_file PRIVATE cxx_std_23)

target_sources(objstore_log_file
    PRIVATE
        log_file_plugin.cpp
)

target_include_directories(objstore_log_file
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../plexdb/log
)
