cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
if(NOT DEFINED PROJECT_NAME)
    set(OBJSTORE_IS_MAIN_PROJECT ON)
else()
    set(OBJSTORE_IS_MAIN_PROJECT OFF)
endif()

project(objstore)

if(OBJSTORE_IS_MAIN_PROJECT)
    option(BUILD_TESTS "Build unit tests" ON)
    option(OBJSTORE_BUILD_SERVER "Build objstore server" ON)
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
# Third part dependencies
# =============================================================================
include(FetchContent)
include(FindPkgConfig)
pkg_check_modules(liburing REQUIRED IMPORTED_TARGET GLOBAL liburing>=2.0)

FetchContent_Declare(lexy URL https://lexy.foonathan.net/download/lexy-src.zip)
FetchContent_MakeAvailable(lexy)

set(LEXY_ENCODING_OF_CHAR lexy::ascii)
set(LEXY_HAS_NTTP ON)
set(LEXY_HAS_CONSTEVAL ON)

if(!PLEXDB_DEBUG)
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

target_include_directories(objstore_modules PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(objstore_modules
    PUBLIC
        FILE_SET CXX_MODULES 
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            engine/types.cppm
            engine/engine.cppm
            engine/schema.cppm
            engine/statements.cppm
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
        plexdb::plexdb
    PRIVATE
        PkgConfig::liburing
)

# Static library
add_library(objstore STATIC)
add_library(plexdb::objstore ALIAS objstore)

target_compile_features(objstore PUBLIC cxx_std_23)
set_target_properties(objstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(objstore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
)

target_sources(objstore
    PRIVATE
        engine/engine.cpp
        engine/schema.cpp
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
        PkgConfig::liburing
        foonathan::lexy::core
        foonathan::lexy::ext
)

# =============================================================================
# Plugins
# =============================================================================
if(PLEXDB_LOG_ENABLED)
    add_library(objstore_log_file SHARED)
    target_compile_features(objstore_log_file PRIVATE cxx_std_23)

    target_sources(objstore_log_file
        PRIVATE
            plugins/log_file/log_file_plugin.cpp
    )

    target_include_directories(objstore_log_file
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../plexdb/log
    )

    add_library(objstore_log_stat SHARED)
    target_compile_features(objstore_log_stat PRIVATE cxx_std_20)

    target_sources(objstore_log_stat
        PRIVATE
            plugins/log_stat/log_stat_plugin.cpp
    )

    target_include_directories(objstore_log_stat
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../plexdb/log
    )
endif()

# =============================================================================
# Executables
# =============================================================================
if(OBJSTORE_BUILD_SERVER)
    add_executable(objstore_server)

    target_sources(objstore_server
        PRIVATE
            entry/main.cpp
    )

    target_link_libraries(objstore_server
        PRIVATE
            plexdb::objstore
    )

    target_include_directories(objstore_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    )

    if(PLEXDB_LOG_ENABLED)
        target_link_options(objstore_server PRIVATE -rdynamic)
    endif()
endif()

# =============================================================================
# Tests
# =============================================================================
if(BUILD_TESTS)
    # Fetch Catch2 only once (parent scope can override this)
    if(NOT TARGET Catch2::Catch2WithMain)
        Include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.11.0
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    add_executable(objstore_tests)
    
    file(GLOB_RECURSE OBJSTORE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
    )
    file(GLOB_RECURSE OBJSTORE_TEST_MODULE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.helper.cppm"
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

    target_include_directories(objstore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
        ${CMAKE_CURRENT_SOURCE_DIR}/../plexdb/log
    )

    target_link_libraries(objstore_tests
        PRIVATE
            plexdb::objstore
            Catch2::Catch2WithMain
    )
endif()