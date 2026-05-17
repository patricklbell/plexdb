cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Project setup
# =============================================================================
project(plexdb_core)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core)
  add_subdirectory(../support/lexy)
  add_subdirectory(../support/tracy)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/options.cmake)

option(plexdb_CORE_ENABLE_IO_URING "Enable using io_uring for io" ON)

# =============================================================================
# Third part dependencies
# =============================================================================
if(plexdb_CORE_ENABLE_IO_URING)
  include(FindPkgConfig)
  pkg_check_modules(liburing REQUIRED IMPORTED_TARGET GLOBAL liburing>=2.0)
endif()

# =============================================================================
# Library
# =============================================================================
add_subdirectory(macros)
add_subdirectory(plugin_abi)

add_library(plexdb_core)
add_library(plexdb::core ALIAS plexdb_core)

target_compile_features(plexdb_core PUBLIC cxx_std_23)
set_target_properties(plexdb_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(plexdb_core PRIVATE CORE_ENABLE_IO_URING=$<IF:$<BOOL:${CORE_ENABLE_IO_URING}>,1,0>)

target_include_directories(plexdb_core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

target_sources(plexdb_core
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            aio/aio.cppm
            argparse/argparse.cppm
            arena/arena.cppm
            base/base.cppm
            base/containers.cppm
            base/math.cppm
            base/string.cppm
            base/types.cppm
            blob/implementations/blob.paged.cppm
            blob/implementations/blob.paged.detail.cppm
            blob/implementations/blob.in_memory.cppm
            blob/implementations/blob.in_memory.detail.cppm
            blob/blob.types.cppm
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
            coroutine/debug.cppm
            coroutine/extra.cppm
            dynamic/containers.cppm
            dynamic/function.cppm
            dynamic/tagged_union.cppm
            plugin/plugin.cppm
            os/aio.cppm
            os/core.cppm
            os/os.cppm
            os/signal.cppm
            os/socket.cppm
            os/sysinfo.cppm
            os/thread.cppm
            os/time.cppm
            os/uring.cppm
            tagged_union/tagged_union.cppm
            tcp/tcp.types.cppm
            tcp/tcp.detail.cppm
            tcp/tcp.cppm
            pager/pager.cppm
            pager/pager.types.cppm
            pager/pager.wal.cppm
            shard/shard.cppm
            shard/spsc.cppm
            shard/token.cppm
            third_party/xxhash/xxhash.cppm
            threads/threads.cppm
    PRIVATE
        aio/aio.cpp
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
        plugin/plugin.cpp
        os/aio.cpp
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
        tcp/tcp.types.cpp
        tcp/tcp.detail.cpp
)

target_link_libraries(plexdb_core
    PRIVATE
        plexdb::compiler_config
        plexdb::core::macros
        plexdb::core::plugin_abi
        plexdb::support::tracy
        $<$<BOOL:${CORE_ENABLE_IO_URING}>:PkgConfig::liburing>
        stdc++exp # std::stacktrace may require linking experimental c++ library
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

  target_compile_definitions(plexdb_core PRIVATE CORE_ENABLE_IO_URING=$<IF:$<BOOL:${CORE_ENABLE_IO_URING}>,1,0>)

  file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cpp"
  )
  file(GLOB_RECURSE CORE_TEST_MODULE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/**/*.test.cppm"
  )
  target_sources(core_tests
      PRIVATE
          ${CORE_TEST_SOURCES}
      PUBLIC
          FILE_SET CXX_MODULES
          BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
          FILES
              ${CORE_TEST_MODULE_SOURCES}
  )

  target_link_libraries(core_tests
        PRIVATE
            plexdb::compiler_config
            plexdb::core
            plexdb::core::plugin_abi
            plexdb::support::tracy
            Catch2::Catch2WithMain
            stdc++exp # std::stacktrace may require linking experimental c++ library
    )
endif()
