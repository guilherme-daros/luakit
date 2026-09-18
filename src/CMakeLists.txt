# The demo host. LUNA_EMBED decides how lua/init.lua gets in: "bytecode" and
# "source" bake it into the executable through luakit_embed_script, "file"
# reads it from disk at startup. The top-level CMakeLists derives the default
# from the build type.

if(NOT LUNA_EMBED MATCHES "^(bytecode|source|file)$")
  message(FATAL_ERROR
          "LUNA_EMBED must be bytecode, source or file (got '${LUNA_EMBED}')")
endif()

message(STATUS "luna: lua/init.lua embedding is '${LUNA_EMBED}'")

set(LUNA_LUA_SRC "${CMAKE_SOURCE_DIR}/lua/init.lua")
set(LUNA_LUA_EXAMPLES "${CMAKE_SOURCE_DIR}/lua/examples.lua")

# Each Lua module exposed to scripts lives in packages/. main.cpp is the host
# that registers them.
set(LUNA_PACKAGES packages/luna.cpp packages/tracker.cpp packages/world.cpp)

add_executable(luna_demo main.cpp ${LUNA_PACKAGES})

set_target_properties(luna_demo PROPERTIES OUTPUT_NAME main)

# src/ on the path is what lets main.cpp say "packages/luna.hpp" while the
# package sources say "luna.hpp" relative to themselves.
target_include_directories(luna_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(luna_demo PRIVATE luakit::luakit)

if(LUNA_EMBED STREQUAL "file")
  # Absolute paths, so the binary can be run from any directory. They also tie
  # the executable to this machine's layout, which is the other half of why a
  # release build embeds instead.
  target_compile_definitions(
    luna_demo PRIVATE LUNA_SCRIPT_PATH="${LUNA_LUA_SRC}"
                      LUNA_EXAMPLES_PATH="${LUNA_LUA_EXAMPLES}")
else()
  # Two scripts, so the mod ships as more than one file and still needs no
  # filesystem at run time. Each defines <name>[] and <name>_len in "<name>.h",
  # plus the macro LUAKIT_EMBEDDED_<NAME> that main.cpp switches on.
  luakit_embed_script(
    TARGET luna_demo
    SCRIPT "${LUNA_LUA_SRC}"
    NAME init_lua
    MODE "${LUNA_EMBED}"
    ${LUNA_STRIP_DEBUG_ARG})

  luakit_embed_script(
    TARGET luna_demo
    SCRIPT "${LUNA_LUA_EXAMPLES}"
    NAME examples_lua
    MODE "${LUNA_EMBED}"
    ${LUNA_STRIP_DEBUG_ARG})
endif()
