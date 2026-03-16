cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
if(NOT DEFINED PROJECT_NAME)
    set(SQLSTORE_IS_MAIN_PROJECT ON)
else()
    set(SQLSTORE_IS_MAIN_PROJECT OFF)
endif()

project(sqlstore)

if(SQLSTORE_IS_MAIN_PROJECT)
    option(BUILD_TESTS "Build unit tests" ON)
    option(SQLSTORE_BUILD_SERVER "Build sqlstore server" ON)
    option(PLEXDB_LOG_ENABLED "Enable structured logging hooks" OFF)
    option(PLEXDB_DEBUG "Build debug checks" ON)

    add_subdirectory(../plexdb plexdb)

    if(PLEXDB_LOG_ENABLED)
        add_compile_definitions(PLEXDB_LOG_ENABLED=1)
    endif()
    if(PLEXDB_DEBUG)
        add_compile_definitions(PLEXDB_DEBUG=1)
    endif()
endif()

# =============================================================================
# Library
# =============================================================================
# Modules
add_library(sqlstore_modules STATIC)
add_library(plexdb::sqlstore_modules ALIAS sqlstore_modules)

target_compile_features(sqlstore_modules PUBLIC cxx_std_20)
set_target_properties(sqlstore_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(sqlstore_modules PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(sqlstore_modules
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            engine/engine.cppm
            engine/statements.cppm
            parsers/parsers.cppm
)

target_link_libraries(sqlstore_modules
    PUBLIC
        plexdb::plexdb
)

# Static library
add_library(sqlstore STATIC)
add_library(plexdb::sqlstore ALIAS sqlstore)

target_compile_features(sqlstore PUBLIC cxx_std_20)
set_target_properties(sqlstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(sqlstore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(sqlstore
    PRIVATE
        engine/engine.cpp
        parsers/parsers.cpp
)

target_link_libraries(sqlstore
    PUBLIC
        plexdb::sqlstore_modules
)

# =============================================================================
# Server executable
# =============================================================================
if(SQLSTORE_BUILD_SERVER)
    add_executable(sqlstore_server)

    target_sources(sqlstore_server
        PRIVATE
            entry/main.cpp
    )

    target_link_libraries(sqlstore_server
        PRIVATE
            plexdb::sqlstore
    )

    target_include_directories(sqlstore_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    )
endif()

# =============================================================================
# Tests
# =============================================================================
if(BUILD_TESTS)
    if(NOT TARGET Catch2::Catch2WithMain)
        Include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.11.0
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    add_executable(sqlstore_tests)

    file(GLOB_RECURSE SQLSTORE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
    )
    target_sources(sqlstore_tests
        PRIVATE
            ${SQLSTORE_TEST_SOURCES}
    )

    target_include_directories(sqlstore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    )

    target_link_libraries(sqlstore_tests
        PRIVATE
            plexdb::sqlstore
            Catch2::Catch2WithMain
    )
endif()
