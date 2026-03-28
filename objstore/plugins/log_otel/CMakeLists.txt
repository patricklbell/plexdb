cmake_minimum_required(VERSION 3.28)
project(objstore_log_otel)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# =============================================================================
# Third party dependencies
# =============================================================================
include(FetchContent)

set(WITH_OTLP_GRPC  ON  CACHE BOOL "" FORCE)
set(WITH_OTLP_HTTP  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK  OFF CACHE BOOL "" FORCE)
set(WITH_FUNC_TESTS OFF CACHE BOOL "" FORCE)
set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)
set(WITH_ABI_VERSION_1 OFF CACHE BOOL "" FORCE)
set(WITH_ABI_VERSION_2 ON CACHE BOOL "" FORCE)

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
add_library(objstore_log_otel SHARED)
target_compile_features(objstore_log_otel PRIVATE cxx_std_20)

target_sources(objstore_log_otel
    PRIVATE
        log_otel_plugin.cpp
)

target_include_directories(objstore_log_otel
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../plexdb/log
)

target_link_libraries(objstore_log_otel
    PRIVATE
        opentelemetry_metrics
        opentelemetry_logs
        opentelemetry_exporter_otlp_grpc_metrics
        opentelemetry_exporter_otlp_grpc_log
        opentelemetry_resources
)
