cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/options.cmake)

# =============================================================================
# Third-party dependencies
# =============================================================================
if(NOT TARGET cpp-sort::cpp-sort)
  find_package(cpp-sort CONFIG QUIET)
  if(NOT cpp-sort_FOUND)
    set(CPPSORT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CPPSORT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CPPSORT_ENABLE_ASSERTIONS OFF CACHE BOOL "" FORCE)
    set(CPPSORT_ENABLE_AUDITS OFF CACHE BOOL "" FORCE)

    include(FetchContent)
    FetchContent_Declare(
      cpp-sort
      GIT_REPOSITORY https://github.com/Morwenn/cpp-sort.git
      GIT_TAG        1.16.0
      GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(cpp-sort)
  endif()
endif()

# =============================================================================
# Library
# =============================================================================
add_library(plexdb_support_sort)
add_library(plexdb::support::sort ALIAS plexdb_support_sort)

target_compile_features(plexdb_support_sort PUBLIC cxx_std_23)

target_sources(plexdb_support_sort
    PUBLIC
        FILE_SET CXX_MODULES TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            sort.cppm
)

target_link_libraries(plexdb_support_sort
    PUBLIC
        cpp-sort::cpp-sort
    PRIVATE
        plexdb::compiler_config
        plexdb::core
)
