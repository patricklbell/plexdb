cmake_minimum_required(VERSION 3.28)
project(plexdb_otel_plugin)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../core/plugin_abi plugin_abi)

# =============================================================================
# Third party dependencies
# =============================================================================
set(WITH_OTLP_GRPC        ON  CACHE BOOL "" FORCE)
set(WITH_OTLP_HTTP        OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES         OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK        OFF CACHE BOOL "" FORCE)
set(WITH_FUNC_TESTS       OFF CACHE BOOL "" FORCE)
set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)
set(WITH_ABI_VERSION_1    OFF CACHE BOOL "" FORCE)
set(WITH_ABI_VERSION_2    ON  CACHE BOOL "" FORCE)

# @todo let parent define targets or use installed version
include(FetchContent)
FetchContent_Declare(
    opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG        v1.19.0
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(opentelemetry-cpp)

# =============================================================================
# Library
# =============================================================================
add_library(plexdb_otel_plugin SHARED)
target_compile_features(plexdb_otel_plugin PRIVATE cxx_std_20)

target_sources(plexdb_otel_plugin
    PRIVATE
        otel_plugin.cpp
)

target_link_libraries(plexdb_otel_plugin
    PRIVATE
        plexdb::core::plugin_abi
        opentelemetry_metrics
        opentelemetry_logs
        opentelemetry_exporter_otlp_grpc_metrics
        opentelemetry_exporter_otlp_grpc_log
        opentelemetry_resources
)
