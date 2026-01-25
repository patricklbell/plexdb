cmake_minimum_required(VERSION 3.28)

# =============================================================================
# project
# =============================================================================
project(plexdb)

option(BUILD_TESTS "Build unit tests" OFF)

set(CMAKE_CXX_STANDARD 20)

# disable exceptions and RTTI
if(!BUILD_TESTS)
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHs-c-")
        string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
        string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELEASE}")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")
    endif()
endif()

# =============================================================================
# library
# =============================================================================
add_library(plexdb_modules)
target_include_directories(plexdb_modules PRIVATE src)
target_sources(plexdb_modules
    PUBLIC
        FILE_SET CXX_MODULES FILES
            src/arena/arena.cppm
            src/base/base.cppm
            src/base/containers.cppm
            src/base/math.cppm
            src/base/string.cppm
            src/base/types.cppm
            src/btree/implementations/btree.in_memory.cppm
            src/btree/implementations/btree.paged.cppm
            src/btree/implementations/btree.in_memory.detail.cppm
            src/btree/implementations/btree.paged.detail.cppm
            src/btree/btree.detail.cppm
            src/btree/btree.cppm
            src/btree/btree.node.cppm
            src/btree/btree.print.cppm
            src/btree/btree.types.cppm
            src/os/os.cppm
            src/pager/pager.cppm
)

add_library(plexdb STATIC)
target_include_directories(plexdb PRIVATE src)
target_sources(plexdb
    PRIVATE
        src/arena/arena.cpp
        src/base/math.cpp
        src/base/string.cpp
        src/base/types.cpp
        src/btree/btree.node.cpp
        src/btree/implementations/btree.paged.cpp
        src/btree/implementations/btree.in_memory.detail.cpp
        src/btree/implementations/btree.in_memory.cpp
        src/btree/implementations/btree.paged.detail.cpp
        src/btree/btree.detail.cpp
        src/os/os.cpp
        src/pager/pager.cpp
)

target_link_libraries(plexdb PRIVATE plexdb_modules)

# =============================================================================
# tests
# =============================================================================
if(BUILD_TESTS)
    Include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.11.0
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(plexdb_tests)
    file(GLOB_RECURSE PLEXDB_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/**/*.test.cpp"
    )
    target_sources(plexdb_tests
        PRIVATE
            ${PLEXDB_TEST_SOURCES}
    )

    target_link_libraries(plexdb_tests
        PRIVATE
            plexdb
            Catch2::Catch2WithMain
    )
endif()
