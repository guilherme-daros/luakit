# The demo host. LUNA_EMBED decides how lua/init.lua gets in: "bytecode" and
# "source" bake it into the executable, "file" reads it from disk at startup.
# The top-level CMakeLists derives the default from the build type.

if(NOT LUNA_EMBED MATCHES "^(bytecode|source|file)$")
  message(FATAL_ERROR
          "LUNA_EMBED must be bytecode, source or file (got '${LUNA_EMBED}')")
endif()

message(STATUS "luna: lua/init.lua embedding is '${LUNA_EMBED}'")

set(LUNA_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(LUNA_LUA_SRC "${CMAKE_SOURCE_DIR}/lua/init.lua")

if(LUNA_EMBED STREQUAL "bytecode")
  # Bytecode is tied to the exact Lua build that produced it, so luac must be
  # the one shipped alongside the liblua we link.
  find_program(
    LUAC_EXECUTABLE
    NAMES luac5.4 luac
    REQUIRED)

  set(LUAC_FLAGS "")
  if(LUNA_STRIP_DEBUG)
    set(LUAC_FLAGS -s)
  endif()

  add_custom_command(
    OUTPUT "${LUNA_GEN_DIR}/init.luac"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LUNA_GEN_DIR}"
    COMMAND "${LUAC_EXECUTABLE}" ${LUAC_FLAGS} -o "${LUNA_GEN_DIR}/init.luac"
            "${LUNA_LUA_SRC}"
    DEPENDS "${LUNA_LUA_SRC}"
    COMMENT "Compiling init.lua to bytecode"
    VERBATIM)

  set(LUNA_EMBED_INPUT "${LUNA_GEN_DIR}/init.luac")
elseif(LUNA_EMBED STREQUAL "source")
  # Embed the source text; liblua parses it at startup.
  set(LUNA_EMBED_INPUT "${LUNA_LUA_SRC}")
endif()

if(LUNA_EMBED_INPUT)
  # xxd is only needed when something is actually being embedded, so a build
  # that loads from disk does not require it to be installed.
  find_program(
    XXD_EXECUTABLE
    NAMES xxd
    REQUIRED)

  # xxd takes an output path directly, which avoids needing shell redirection.
  add_custom_command(
    OUTPUT "${LUNA_GEN_DIR}/init_lua.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LUNA_GEN_DIR}"
    COMMAND "${XXD_EXECUTABLE}" -i -n init_lua "${LUNA_EMBED_INPUT}"
            "${LUNA_GEN_DIR}/init_lua.h"
    DEPENDS "${LUNA_EMBED_INPUT}"
    COMMENT "Embedding init.lua (${LUNA_EMBED}) as a byte array"
    VERBATIM)
endif()

# Each Lua module exposed to scripts lives in packages/. main.cpp is the host
# that registers them.
set(LUNA_PACKAGES packages/luna.cpp packages/tracker.cpp)

add_executable(luna_demo main.cpp ${LUNA_PACKAGES})

set_target_properties(luna_demo PROPERTIES OUTPUT_NAME main)

# src/ on the path is what lets main.cpp say "packages/luna.hpp" while the
# package sources say "luna.hpp" relative to themselves.
target_include_directories(luna_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(luna_demo PRIVATE luakit::luakit)

if(LUNA_EMBED_INPUT)
  # Listing the generated header as a source is what ties it to the target and
  # gets the custom commands above run.
  target_sources(luna_demo PRIVATE "${LUNA_GEN_DIR}/init_lua.h")
  target_include_directories(luna_demo PRIVATE "${LUNA_GEN_DIR}")
  target_compile_definitions(luna_demo PRIVATE LUNA_EMBEDDED_SCRIPT)
else()
  # An absolute path, so the binary can be run from any directory. It also
  # ties the executable to this machine's layout, which is the other half of
  # why a release build embeds instead.
  target_compile_definitions(luna_demo
                             PRIVATE LUNA_SCRIPT_PATH="${LUNA_LUA_SRC}")
endif()
