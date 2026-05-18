cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
project(resp)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core core)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../support/tracy tracy)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/options.cmake)

option(KEYVALUE_ENABLE_SERVER "Build keyvalue server" ON)

# =============================================================================
# Library
# =============================================================================
add_library(plexdb_keyvalue)
add_library(plexdb::keyvalue ALIAS plexdb_keyvalue)

target_compile_features(plexdb_keyvalue PUBLIC cxx_std_23)
set_target_properties(plexdb_keyvalue PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(plexdb_keyvalue
    PUBLIC
        FILE_SET CXX_MODULES TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            engine/engine.cppm
            engine/statements.cppm
            glob/glob.cppm
            log/log.cppm
            parsers/parsers.cppm
            resp/resp.cppm
            resp/resp.protocol.cppm
    PRIVATE
        engine/engine.cpp
        glob/glob.cpp
        log/log.cpp
        parsers/parsers.cpp
        resp/resp.cpp
        resp/resp.protocol.cpp
)


target_link_libraries(plexdb_keyvalue
    PRIVATE
        plexdb::compiler_config
        plexdb::core
        plexdb::core::macros
        plexdb::support::tracy
)

# =============================================================================
# Executables
# =============================================================================
if(KEYVALUE_ENABLE_SERVER)
  add_executable(plexdb_keyvalue_server)

  target_sources(plexdb_keyvalue_server
      PRIVATE
          entry/main.cpp
  )

  target_link_libraries(plexdb_keyvalue_server
      PRIVATE
          plexdb::compiler_config
          plexdb::core::macros
          plexdb::keyvalue
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

  add_executable(keyvalue_tests)

  file(GLOB_RECURSE KEYVALUE_TEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/*.test.cpp"
  )
  file(GLOB_RECURSE KEYVALUE_TEST_MODULE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/*.test.cppm"
  )
  target_sources(keyvalue_tests
      PRIVATE
          ${KEYVALUE_TEST_SOURCES}
      PUBLIC
          FILE_SET CXX_MODULES TYPE CXX_MODULES
          BASE_DIRS
              ${CMAKE_CURRENT_SOURCE_DIR}
              ${CMAKE_CURRENT_SOURCE_DIR}/../core
          FILES
              ${KEYVALUE_TEST_MODULE_SOURCES}
  )

  target_link_libraries(keyvalue_tests
      PRIVATE
          plexdb::compiler_config
          plexdb::core::plugin_abi
          plexdb::core::test_macros
          plexdb::keyvalue
          Catch2::Catch2WithMain
          stdc++exp
  )
endif()
