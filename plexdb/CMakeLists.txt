cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
if(NOT DEFINED PROJECT_NAME)
    set(PLEXDB_IS_MAIN_PROJECT ON)
else()
    set(PLEXDB_IS_MAIN_PROJECT OFF)
endif()

project(plexdb)

if(PLEXDB_IS_MAIN_PROJECT)
    option(BUILD_TESTS "Build unit tests" ON)
    option(PLEXDB_LOG_ENABLED "Enable structured logging hooks" OFF)
    option(PLEXDB_DEBUG "Build debug checks" ON)

    if(PLEXDB_DEBUG)
        add_compile_definitions(PLEXDB_DEBUG=1)
    endif()
    if(PLEXDB_LOG_ENABLED)
        add_compile_definitions(PLEXDB_LOG_ENABLED=1)
    endif()
endif()

# =============================================================================
# Library
# =============================================================================
# Modules
add_library(plexdb_modules STATIC)
add_library(plexdb::plexdb_modules ALIAS plexdb_modules)

target_compile_features(plexdb_modules PUBLIC cxx_std_20)
set_target_properties(plexdb_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(plexdb_modules PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/log
)

target_sources(plexdb_modules
    PUBLIC
        FILE_SET CXX_MODULES 
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            argparse/argparse.cppm
            arena/arena.cppm
            base/base.cppm
            base/containers.cppm
            base/math.cppm
            base/string.cppm
            base/types.cppm
            blob/implementations/blob.paged.cppm
            blob/implementations/blob.paged.detail.cppm
            blob/blob.cppm
            btree/implementations/btree.paged.cppm
            btree/implementations/btree.paged.detail.cppm
            btree/implementations/btree.in_memory.cppm
            btree/implementations/btree.in_memory.detail.cppm
            btree/btree.detail.cppm
            btree/btree.cppm
            btree/btree.node.cppm
            btree/btree.print.cppm
            btree/btree.types.cppm
            os/containers.cppm
            os/core.cppm
            third_party/xxhash/xxhash.cppm
            os/function.cppm
            os/os.cppm
            os/signal.cppm
            os/socket.cppm
            os/sysinfo.cppm
            os/uring.cppm
            pager/pager.cppm
            shard/shard.cppm
            shard/spsc.cppm
            shard/token.cppm
            threads/threads.cppm
            tagged_union/tagged_union.cppm
            log/log.cppm
)

# Static library
add_library(plexdb STATIC)
add_library(plexdb::plexdb ALIAS plexdb)

target_compile_features(plexdb PUBLIC cxx_std_20)
set_target_properties(plexdb PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(plexdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../macros
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/log
)

target_sources(plexdb
    PRIVATE
        argparse/argparse.cpp
        arena/arena.cpp
        base/math.cpp
        base/string.cpp
        base/types.cpp
        blob/implementations/blob.paged.cpp
        blob/implementations/blob.paged.detail.cpp
        btree/implementations/btree.paged.cpp
        btree/implementations/btree.in_memory.detail.cpp
        btree/implementations/btree.in_memory.cpp
        btree/implementations/btree.paged.detail.cpp
        btree/btree.detail.cpp
        btree/btree.node.cpp
        os/core.cpp
        os/signal.cpp
        os/socket.cpp
        os/sysinfo.cpp
        os/uring.cpp
        pager/pager.cpp
        threads/threads.cpp
        log/log.cpp
)

target_link_libraries(plexdb PUBLIC plexdb_modules)

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

    add_executable(plexdb_tests)
    
    file(GLOB_RECURSE PLEXDB_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
    )
    target_sources(plexdb_tests PRIVATE ${PLEXDB_TEST_SOURCES})
    target_link_libraries(plexdb_tests
        PRIVATE
            plexdb::plexdb
            Catch2::Catch2WithMain
    )
endif()