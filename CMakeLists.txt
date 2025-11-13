cmake_minimum_required(VERSION 3.28)

# =============================================================================
# project
# =============================================================================
project(plexdb)

option(BUILD_TESTS "Build unit tests" OFF)

set(CMAKE_CXX_STANDARD 20)

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
            src/btree/btree.cppm
            src/btree/btree.detail.cppm
            src/os/os.cppm
)

add_library(plexdb STATIC)
target_include_directories(plexdb PRIVATE src)
target_sources(plexdb
    PRIVATE
        src/arena/arena.cpp
        src/base/base.cpp
        src/btree/btree.cpp
        src/btree/btree.detail.cpp
        src/os/os.cpp
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

    add_executable(tests)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/**/*.test.cpp"
    )
    target_sources(tests
        PRIVATE
            ${TEST_SOURCES}
    )

    target_link_libraries(tests
        PRIVATE
            plexdb
            Catch2::Catch2WithMain
    )
endif()
