# The demo host, with lua/init.lua compiled to bytecode and baked into the
# executable. Mirrors what the Makefile does.

find_program(
  LUAC_EXECUTABLE
  NAMES luac5.4 luac
  REQUIRED)
find_program(
  XXD_EXECUTABLE
  NAMES xxd
  REQUIRED)

set(LUNA_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(LUNA_LUA_SRC "${CMAKE_SOURCE_DIR}/lua/init.lua")

# Bytecode is tied to the exact Lua build that produced it, so luac must be the
# one shipped alongside the liblua we link. -s strips line numbers and local
# names; drop it to get those back in tracebacks.
add_custom_command(
  OUTPUT "${LUNA_GEN_DIR}/init.luac"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${LUNA_GEN_DIR}"
  COMMAND "${LUAC_EXECUTABLE}" -s -o "${LUNA_GEN_DIR}/init.luac" "${LUNA_LUA_SRC}"
  DEPENDS "${LUNA_LUA_SRC}"
  COMMENT "Compiling init.lua to bytecode"
  VERBATIM)

# xxd takes an output path directly, which avoids needing shell redirection.
add_custom_command(
  OUTPUT "${LUNA_GEN_DIR}/init_lua.h"
  COMMAND "${XXD_EXECUTABLE}" -i -n init_lua "${LUNA_GEN_DIR}/init.luac"
          "${LUNA_GEN_DIR}/init_lua.h"
  DEPENDS "${LUNA_GEN_DIR}/init.luac"
  COMMENT "Embedding init.luac as a byte array"
  VERBATIM)

add_executable(luna_demo main.cpp luna.cpp tracker.cpp "${LUNA_GEN_DIR}/init_lua.h")

set_target_properties(luna_demo PROPERTIES OUTPUT_NAME main)

target_include_directories(luna_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                             "${LUNA_GEN_DIR}")

target_link_libraries(luna_demo PRIVATE luakit::luakit)
