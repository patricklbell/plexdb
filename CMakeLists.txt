cmake_minimum_required(VERSION 3.28)

project(plexdb)

set(CMAKE_CXX_STANDARD 20)

# static library
add_library(plexdb)

target_include_directories(plexdb PRIVATE src)
target_sources(plexdb
    PRIVATE
        src/arena/arena.cpp
        src/btree/btree.cpp
        src/common/std.cpp
        src/os/os.cpp
    
    PUBLIC
        FILE_SET modules
        TYPE CXX_MODULES
        FILES
            src/arena/arena.cppm
            src/btree/btree.cppm
            src/common/common.cppm
            src/common/std.cppm
            src/os/os.cppm
)

# testing
Include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.11.0
)

FetchContent_MakeAvailable(Catch2)

add_executable(tests)
target_sources(tests
    PRIVATE
        src/btree/tests/insert.cpp
)
target_link_libraries(tests
    PRIVATE
        plexdb
        Catch2::Catch2WithMain
)
