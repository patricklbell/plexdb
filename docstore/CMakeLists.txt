cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
if(NOT DEFINED PROJECT_NAME)
    set(DOCSTORE_IS_MAIN_PROJECT ON)
else()
    set(DOCSTORE_IS_MAIN_PROJECT OFF)
endif()

project(docstore)

if(DOCSTORE_IS_MAIN_PROJECT)
    option(BUILD_TESTS "Build unit tests" ON)
    option(DOCSTORE_BUILD_SERVER "Build docstore server" ON)
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
add_library(docstore_modules STATIC)
add_library(plexdb::docstore_modules ALIAS docstore_modules)

target_compile_features(docstore_modules PUBLIC cxx_std_20)
set_target_properties(docstore_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(docstore_modules PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(docstore_modules
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            engine/engine.cppm
            engine/statements.cppm
            parsers/parsers.cppm
)

target_link_libraries(docstore_modules
    PUBLIC
        plexdb::plexdb
)

# Static library
add_library(docstore STATIC)
add_library(plexdb::docstore ALIAS docstore)

target_compile_features(docstore PUBLIC cxx_std_20)
set_target_properties(docstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(docstore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(docstore
    PRIVATE
        engine/engine.cpp
        parsers/parsers.cpp
)

target_link_libraries(docstore
    PUBLIC
        plexdb::docstore_modules
)

# =============================================================================
# Server executable
# =============================================================================
if(DOCSTORE_BUILD_SERVER)
    add_executable(docstore_server)

    target_sources(docstore_server
        PRIVATE
            entry/main.cpp
    )

    target_link_libraries(docstore_server
        PRIVATE
            plexdb::docstore
    )

    target_include_directories(docstore_server PRIVATE
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

    add_executable(docstore_tests)

    file(GLOB_RECURSE DOCSTORE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
    )
    target_sources(docstore_tests
        PRIVATE
            ${DOCSTORE_TEST_SOURCES}
    )

    target_include_directories(docstore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    )

    target_link_libraries(docstore_tests
        PRIVATE
            plexdb::docstore
            Catch2::Catch2WithMain
    )
endif()
