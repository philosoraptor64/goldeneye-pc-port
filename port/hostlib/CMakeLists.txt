# Host-side helpers that need plain system headers (the game's include path
# shadows <stdlib.h>/<string.h>): re-entry (start/stop several times per process).
add_library(ge007_host STATIC reentry.c)
target_include_directories(ge007_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ge007_host PROPERTIES C_STANDARD 11)

# i64 module glue (n64_game_* exports), linked into the shared game library
# as objects so the exported symbols are always there.
if(GE_I64_MODULE)
  add_library(ge007_i64glue OBJECT ge_i64_module.c)
  target_include_directories(ge007_i64glue PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/port/convert" "${CMAKE_BINARY_DIR}/generated")
  target_compile_definitions(ge007_i64glue PRIVATE GE_I64_MODULE=1)
  set_target_properties(ge007_i64glue PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
endif()
