cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
if(NOT DEFINED PROJECT_NAME)
    set(GRAPHSTORE_IS_MAIN_PROJECT ON)
else()
    set(GRAPHSTORE_IS_MAIN_PROJECT OFF)
endif()

project(graphstore)

if(GRAPHSTORE_IS_MAIN_PROJECT)
    option(BUILD_TESTS "Build unit tests" ON)
    option(GRAPHSTORE_BUILD_SERVER "Build graphstore server" ON)
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
add_library(graphstore_modules STATIC)
add_library(plexdb::graphstore_modules ALIAS graphstore_modules)

target_compile_features(graphstore_modules PUBLIC cxx_std_20)
set_target_properties(graphstore_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(graphstore_modules PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(graphstore_modules
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            engine/engine.cppm
            engine/statements.cppm
            parsers/parsers.cppm
)

target_link_libraries(graphstore_modules
    PUBLIC
        plexdb::plexdb
)

# Static library
add_library(graphstore STATIC)
add_library(plexdb::graphstore ALIAS graphstore)

target_compile_features(graphstore PUBLIC cxx_std_20)
set_target_properties(graphstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(graphstore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(graphstore
    PRIVATE
        engine/engine.cpp
        parsers/parsers.cpp
)

target_link_libraries(graphstore
    PUBLIC
        plexdb::graphstore_modules
)

# =============================================================================
# Server executable
# =============================================================================
if(GRAPHSTORE_BUILD_SERVER)
    add_executable(graphstore_server)

    target_sources(graphstore_server
        PRIVATE
            entry/main.cpp
    )

    target_link_libraries(graphstore_server
        PRIVATE
            plexdb::graphstore
    )

    target_include_directories(graphstore_server PRIVATE
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

    add_executable(graphstore_tests)

    file(GLOB_RECURSE GRAPHSTORE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
    )
    target_sources(graphstore_tests
        PRIVATE
            ${GRAPHSTORE_TEST_SOURCES}
    )

    target_include_directories(graphstore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    )

    target_link_libraries(graphstore_tests
        PRIVATE
            plexdb::graphstore
            Catch2::Catch2WithMain
    )
endif()
