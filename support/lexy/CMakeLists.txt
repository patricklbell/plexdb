cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/compiler_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/options.cmake)

# =============================================================================
# Third-party dependencies
# =============================================================================
set(LEXY_ENCODING_OF_CHAR lexy::ascii)
set(LEXY_HAS_NTTP ON)
set(LEXY_HAS_CONSTEVAL ON)
if(NOT PLEXDB_DEBUG)
  set(LEXY_ENABLE_ASSERT OFF)
endif()

if(NOT TARGET foonathan::lexy)
  include(FetchContent)
  FetchContent_Declare(lexy URL https://lexy.foonathan.net/download/lexy-src.zip)
  FetchContent_MakeAvailable(lexy)
endif()

# =============================================================================
# Library
# =============================================================================
add_library(plexdb_support_lexy)
add_library(plexdb::support::lexy ALIAS plexdb_support_lexy)

target_compile_features(plexdb_support_lexy PUBLIC cxx_std_23)

target_include_directories(plexdb_support_lexy
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_sources(plexdb_support_lexy
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            error_callback.cppm
)

target_link_libraries(plexdb_support_lexy
    PRIVATE
        plexdb::compiler_config
        plexdb::core
        foonathan::lexy::core
)
