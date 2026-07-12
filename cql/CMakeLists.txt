cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
project(cql)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core core)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../support/lexy lexy)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../support/sort sort)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../support/tracy tracy)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/options.cmake)

option(PLEXDB_CQL_ENABLE_SERVER "Build cql server" ON)

add_subdirectory(test_macros)

# =============================================================================
# Third party dependencies
# =============================================================================
if(NOT TARGET lz4::lz4)
  include(FetchContent)
  FetchContent_Declare(
      lz4
      GIT_REPOSITORY https://github.com/lz4/lz4.git
      GIT_TAG        v1.10.0
      GIT_SHALLOW    TRUE
      SOURCE_SUBDIR  build/cmake
  )
  set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lz4)
endif()

# =============================================================================
# Library
# =============================================================================
add_library(plexbd_cql)
add_library(plexdb::cql ALIAS plexbd_cql)

target_compile_features(plexbd_cql PUBLIC cxx_std_23)
set_target_properties(plexbd_cql PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(plexbd_cql
    PUBLIC
        FILE_SET CXX_MODULES TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            crc/crc.cppm
            engine/engine.cppm
            engine/clustering_compare.cppm
            engine/column_value.cppm
            engine/evaluator.cppm
            engine/io.cppm
            engine/io.codec.cppm
            engine/io.evaluator.cppm
            engine/io.types.cppm
            engine/generated/io_codec_wire.cppm
            engine/generated/token_wire.cppm
            engine/it.cppm
            engine/key.cppm
            engine/planner.cppm
            engine/schema.cppm
            engine/schema.types.cppm
            engine/system_schema.cppm
            engine/statements.cppm
            engine/token.cppm
            engine/types.cppm
            engine/virtual_table.cppm
            log/log.cppm
            parsers/parsers.cppm
            native/native.cppm
            third_party/lz4/lz4.cppm
            third_party/murmur3/murmur3.cppm

    PRIVATE
        engine/engine.cpp
        engine/clustering_compare.cpp
        engine/column_value.cpp
        engine/evaluator.cpp
        engine/io.codec.cpp
        engine/io.evaluator.cpp
        engine/generated/io_codec_wire.cpp
        engine/generated/io_codec_sync.cpp
        engine/generated/token_wire.cpp
        engine/it.cpp
        engine/planner.cpp
        engine/schema.cpp
        engine/schema.types.cpp
        engine/system_schema.cpp
        engine/token.cpp
        log/log.cpp
        parsers/parsers.cpp
        native/native.cpp
        third_party/lz4/lz4.cpp
        third_party/murmur3/murmur3.c
)

# Vendored Murmur3 (PeterScott/murmur3) is plain C; isolate it from the C++
# warning set the rest of the library uses.
set_source_files_properties(third_party/murmur3/murmur3.c PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-Wno-implicit-fallthrough;-Wno-unused-variable;-Wno-unused-but-set-variable"
)

target_link_libraries(plexbd_cql
    PUBLIC
        plexdb::core
    PRIVATE
        plexdb::compiler_config
        plexdb::core::macros
        plexdb::support::lexy
        plexdb::support::sort
        plexdb::support::tracy
        foonathan::lexy::core # @note pulled in by support
        foonathan::lexy::ext
        lz4_static
)

# =============================================================================
# Executables
# =============================================================================
if(PLEXDB_CQL_ENABLE_SERVER)
  add_executable(plexdb_cql_server)

  target_sources(plexdb_cql_server
        PRIVATE
            entry/main.cpp
    )

  target_link_libraries(plexdb_cql_server
        PRIVATE
            plexdb::compiler_config
            plexdb::core::macros
            plexdb::cql
            plexdb::support::tracy
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

  add_executable(cql_tests)

  file(GLOB_RECURSE CQL_TEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/*.test.cpp"
  )
  file(GLOB_RECURSE CQL_TEST_MODULE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/*.test.cppm"
  )
  target_sources(cql_tests
      PRIVATE
          ${CQL_TEST_SOURCES}
      PUBLIC
          FILE_SET CXX_MODULES TYPE CXX_MODULES
          BASE_DIRS
              ${CMAKE_CURRENT_SOURCE_DIR}
              ${CMAKE_CURRENT_SOURCE_DIR}/../core
          FILES
              ${CQL_TEST_MODULE_SOURCES}
  )

  target_link_libraries(cql_tests
      PRIVATE
          plexdb::compiler_config
          plexdb::core::macros
          plexdb::core::plugin_abi
          plexdb::core::test_macros
          cql::test_macros
          stdc++exp # std::stacktrace may require linking experimental c++ library
          plexdb::cql
          Catch2::Catch2WithMain
  )
endif()
