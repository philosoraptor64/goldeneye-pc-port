# Host-side helpers that need plain system headers (the game's include path
# shadows <stdlib.h>/<string.h>): re-entry (start/stop several times per process).
add_library(ge007_host STATIC reentry.c)
target_include_directories(ge007_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ge007_host PROPERTIES C_STANDARD 11)
