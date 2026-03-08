cmake_minimum_required(VERSION 3.28)

project(plexdb_workspace)

# =============================================================================
# Options
# =============================================================================
option(BUILD_TESTS "Build unit tests" OFF)
option(DISABLE_EXCEPTIONS "Disable exceptions globally" ON)
option(PLEXDB_DEBUG "Build debug checks" ON)
option(PLEXDB_LOG_ENABLED "Enable structured logging hooks" OFF)

option(OBJSTORE_BUILD "Build objstore" ON)
option(OBJSTORE_BUILD_SERVER "Build objstore server" ON)

# =============================================================================
# Global compiler flags
# =============================================================================
if(DISABLE_EXCEPTIONS)
    if(MSVC)
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHs-c->" "$<$<COMPILE_LANGUAGE:CXX>:/GR->")
        # remove default exception handling flags
        string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REPLACE "/GR" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/GR" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
        string(REPLACE "/GR" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>" "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
    endif()
endif()

# =============================================================================
# Build
# =============================================================================
add_subdirectory(plexdb)
if(OBJSTORE_BUILD)
    add_subdirectory(objstore)
endif()