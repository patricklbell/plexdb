cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
project(core)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/options.cmake)

option(CORE_ENABLE_IO_URING "Enable using io_uring for io" ON)

# =============================================================================
# Third part dependencies
# =============================================================================
if(CORE_ENABLE_IO_URING)
  include(FindPkgConfig)
  pkg_check_modules(liburing REQUIRED IMPORTED_TARGET GLOBAL liburing>=2.0)
endif()

# =============================================================================
# Profiling
# =============================================================================
if(PLEXDB_ENABLE_TRACY_PROFILER)
  if(NOT TARGET TracyClient)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_FIBERS ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)

    include(FetchContent)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY  https://github.com/wolfpld/tracy.git
        GIT_TAG         master
        GIT_SHALLOW     TRUE
        GIT_PROGRESS    TRUE
    )
    FetchContent_MakeAvailable(tracy)
  endif()
endif()

add_library(profiling INTERFACE)
add_library(plexdb::profiling ALIAS profiling)
target_compile_definitions(profiling
    INTERFACE
        $<$<BOOL:${PLEXDB_ENABLE_TRACY_PROFILER}>:PLEXDB_ENABLE_TRACY_PROFILER=1>
)
target_include_directories(profiling
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy
)
target_link_libraries(profiling
    INTERFACE
        $<$<BOOL:${PLEXDB_ENABLE_TRACY_PROFILER}>:TracyClient>
)

# =============================================================================
# Library
# =============================================================================
# Modules
add_library(core_modules STATIC)
add_library(plexdb::core_modules ALIAS core_modules)

target_compile_features(core_modules PUBLIC cxx_std_23)
set_target_properties(core_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(core_modules PRIVATE CORE_ENABLE_IO_URING=$<IF:$<BOOL:${CORE_ENABLE_IO_URING}>,1,0>)

target_include_directories(core_modules
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/log
)

target_sources(core_modules
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
            coroutine/base.cppm
            coroutine/coroutine.cppm
            coroutine/extra.cppm
            log/log.cppm
            os/containers.cppm
            os/core.cppm
            os/dynamic_tagged_union.cppm
            os/function.cppm
            os/os.cppm
            os/signal.cppm
            os/socket.cppm
            os/sysinfo.cppm
            os/thread.cppm
            os/time.cppm
            os/uring.cppm
            tagged_union/tagged_union.cppm
            pager/pager.cppm
            pager/pager.types.cppm
            pager/pager.wal.cppm
            shard/shard.cppm
            shard/spsc.cppm
            shard/token.cppm
            third_party/xxhash/xxhash.cppm
            threads/threads.cppm
)

target_link_libraries(core_modules
    PRIVATE
        plexdb::compiler_config
        plexdb::profiling
)

# Static library
add_library(core STATIC)
add_library(plexdb::core ALIAS core)

target_compile_features(core PUBLIC cxx_std_23)
set_target_properties(core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(core PRIVATE CORE_ENABLE_IO_URING=$<IF:$<BOOL:${CORE_ENABLE_IO_URING}>,1,0>)

target_include_directories(core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../macros
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/log
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy
)

target_sources(core
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
        log/log.cpp
        os/core.cpp
        os/signal.cpp
        os/socket.cpp
        os/sysinfo.cpp
        os/thread.cpp
        os/time.cpp
        os/uring.cpp
        pager/pager.cpp
        pager/pager.wal.cpp
        threads/threads.cpp
)

target_link_libraries(core
    PUBLIC
        core_modules
    PRIVATE
        plexdb::compiler_config
        $<$<BOOL:${CORE_ENABLE_IO_URING}>:PkgConfig::liburing>
        plexdb::profiling
)

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

  add_executable(core_tests)

  target_compile_definitions(core PRIVATE CORE_ENABLE_IO_URING=$<IF:$<BOOL:${CORE_ENABLE_IO_URING}>,1,0>)

  file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
    )
  target_sources(core_tests
        PRIVATE
            ${CORE_TEST_SOURCES}
    )

  target_include_directories(core_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/log
            ${CMAKE_CURRENT_SOURCE_DIR}/test
    )

  target_link_libraries(core_tests
        PRIVATE
            plexdb::compiler_config
            stdc++exp # std::stacktrace may require link experimental c++ library
            plexdb::core
            Catch2::Catch2WithMain
    )
endif()
