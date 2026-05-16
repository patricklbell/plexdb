include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/options.cmake)

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

add_library(plexdb_support_tracy INTERFACE)
add_library(plexdb::support::tracy ALIAS plexdb_support_tracy)
target_compile_definitions(plexdb_support_tracy
    INTERFACE
        $<$<BOOL:${PLEXDB_ENABLE_TRACY_PROFILER}>:PLEXDB_ENABLE_TRACY_PROFILER=1>
)
target_include_directories(plexdb_support_tracy
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(plexdb_support_tracy
    INTERFACE
        $<$<BOOL:${PLEXDB_ENABLE_TRACY_PROFILER}>:TracyClient>
)
