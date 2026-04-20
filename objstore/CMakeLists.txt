cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
project(objstore)

if(NOT TARGET plexdb::core)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/options.cmake)

option(OBJSTORE_ENABLE_SERVER "Build objstore server" ON)

# =============================================================================
# Third part dependencies
# =============================================================================
set(LEXY_ENCODING_OF_CHAR lexy::ascii)
set(LEXY_HAS_NTTP ON)
set(LEXY_HAS_CONSTEVAL ON)

# @todo use installed library (may require cmake configuration files to be installed)
if(NOT TARGET foonathan::lexy)
    include(FetchContent)
    FetchContent_Declare(lexy URL https://lexy.foonathan.net/download/lexy-src.zip)
    FetchContent_MakeAvailable(lexy)
endif()

if(NOT PLEXDB_DEBUG)
    set(LEXY_ENABLE_ASSERT OFF)
endif()

# =============================================================================
# Library
# =============================================================================
# Modules
add_library(objstore_modules STATIC)
add_library(plexdb::objstore_modules ALIAS objstore_modules)

target_compile_features(objstore_modules PUBLIC cxx_std_23)
set_target_properties(objstore_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(objstore_modules
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
        ${CMAKE_CURRENT_SOURCE_DIR}/../test
)

target_sources(objstore_modules
    PUBLIC
        FILE_SET CXX_MODULES 
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            engine/engine.cppm
            engine/evaluator.cppm
            engine/io.cppm
            engine/schema.cppm
            engine/system_schema.cppm
            engine/statements.cppm
            engine/types.cppm
            engine/virtual_table.cppm
            log/log.cppm
            parsers/parsers.cppm
            tcp/tcp.cppm
            tcp/tcp.detail.cppm
            tcp/tcp.types.cppm
            native/native.cppm
            repl/repl.cppm
)

target_link_libraries(objstore_modules
    PUBLIC
        plexdb::core
    PRIVATE
        plexdb::compiler_config
)

# Static library
add_library(objstore STATIC)
add_library(plexdb::objstore ALIAS objstore)

target_compile_features(objstore PUBLIC cxx_std_23)
set_target_properties(objstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(objstore
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(objstore
    PRIVATE
        engine/engine.cpp
        engine/schema.cpp
        engine/system_schema.cpp
        log/log.cpp
        parsers/parsers.cpp
        tcp/tcp.detail.cpp
        tcp/tcp.types.cpp
        native/native.cpp
        repl/repl.cpp
)

target_link_libraries(objstore
    PUBLIC
        plexdb::objstore_modules
    PRIVATE
        plexdb::compiler_config
        foonathan::lexy::core
        foonathan::lexy::ext
)

# =============================================================================
# Executables
# =============================================================================
if(OBJSTORE_ENABLE_SERVER)
    add_executable(objstore_server)

    target_sources(objstore_server
        PRIVATE
            entry/main.cpp
    )

    target_link_libraries(objstore_server
        PRIVATE
            plexdb::compiler_config
            plexdb::objstore
    )

    target_include_directories(objstore_server
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    )
endif()

# =============================================================================
# Tests
# =============================================================================
if(PLEXDB_ENABLE_TESTS)
    find_package(Catch2 3.13.0 QUIET)
    if(NOT TARGET Catch2::Catch2WithMain)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.13.0
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    add_executable(objstore_tests)
    
    file(GLOB_RECURSE OBJSTORE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/*.test.cpp"
    )
    file(GLOB_RECURSE OBJSTORE_TEST_MODULE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/*.test.cppm"
    )
    target_sources(objstore_tests
        PRIVATE
            ${OBJSTORE_TEST_SOURCES}
        PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES
                ${OBJSTORE_TEST_MODULE_SOURCES}
    )

    target_include_directories(objstore_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../macros
            ${CMAKE_CURRENT_SOURCE_DIR}/../core/log
            ${CMAKE_CURRENT_SOURCE_DIR}/../core/test
    )

    target_link_libraries(objstore_tests
        PRIVATE
            plexdb::compiler_config
            stdc++exp # std::stacktrace may require link experimental c++ library
            plexdb::objstore
            Catch2::Catch2WithMain
    )
endif()