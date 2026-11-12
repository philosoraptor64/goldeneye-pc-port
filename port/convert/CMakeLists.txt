# Native sidecar converter (see ge_convert.h). Plain system headers only.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GE_CONVERT_TABLES "${CMAKE_CURRENT_BINARY_DIR}/ge_convert_tables.h")
file(GLOB_RECURSE GE_CONVERT_HEADER_INPUTS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/assets/*odelfileheader.inc.c" "${CMAKE_SOURCE_DIR}/assets/*ODELFILEHEADER.inc.c")
add_custom_command(
  OUTPUT "${GE_CONVERT_TABLES}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools_pc/gen_convert_tables.py"
          "${ROMID}" "${GE_CONVERT_TABLES}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools_pc/gen_convert_tables.py"
          "${CMAKE_SOURCE_DIR}/scripts/filelist.${ASSET_REGION}.csv"
          "${CMAKE_SOURCE_DIR}/assets/obseg/file_resource_table.inc.c"
          ${GE_CONVERT_HEADER_INPUTS}
  VERBATIM)
add_library(ge007_convert STATIC ge_convert.c "${GE_CONVERT_TABLES}")
target_include_directories(ge007_convert PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ge007_convert PUBLIC z)
set_target_properties(ge007_convert PROPERTIES C_STANDARD 11)

# Host check tool: ge007-convert-native <rom.z64> <out_root>
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_executable(ge007-convert-native ge_convert_cli.c)
  target_link_libraries(ge007-convert-native PRIVATE ge007_convert)
endif()
