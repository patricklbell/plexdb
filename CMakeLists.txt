cmake_minimum_required(VERSION 3.28)

project(plexdb)

# =============================================================================
# Options
# =============================================================================
option(OBJSTORE_ENABLE "Build objstore" ON)

# =============================================================================
# Build
# =============================================================================
add_subdirectory(core)
if(OBJSTORE_ENABLE)
  add_subdirectory(objstore)
endif()
