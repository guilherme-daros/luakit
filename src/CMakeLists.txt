# The demo host. LUNA_EMBED decides how lua/ gets in: "bytecode" and "source"
# bake the scripts into the executable through luakit_embed_dir, "file" reads
# them from disk at startup. The top-level CMakeLists derives the default
# from the build type.

if(NOT LUNA_EMBED MATCHES "^(bytecode|source|file)$")
  message(FATAL_ERROR
          "LUNA_EMBED must be bytecode, source or file (got '${LUNA_EMBED}')")
endif()

message(STATUS "luna: lua/ embedding is '${LUNA_EMBED}'")

# Each Lua module exposed to scripts lives in packages/. main.cpp is the host
# that registers them.
set(LUNA_PACKAGES
    packages/luna.cpp
    packages/tracker.cpp
    packages/world/world.cpp
    packages/world/vec2.cpp
    packages/world/entity.cpp
    packages/world/creature.cpp)

add_executable(luna_demo main.cpp demo.cpp ${LUNA_PACKAGES})

set_target_properties(luna_demo PROPERTIES OUTPUT_NAME main)

# src/ on the path is what lets main.cpp say "packages/luna.hpp" while the
# package sources say "luna.hpp" relative to themselves.
target_include_directories(luna_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(luna_demo PRIVATE luakit::luakit)

# Every *.lua under lua/ becomes an entry in luakit::file::script(path), path
# being relative to CMAKE_SOURCE_DIR (e.g. "lua/init.lua"). demo.cpp looks
# scripts up by that name; it does not know or care which LUNA_EMBED mode
# produced them. STUB_DIR is what keeps src/lsp_stub/file_scripts.hpp
# (checked into git) in sync, so an LSP has something to resolve
# luakit::file::script() against before the project has been configured even
# once.
luakit_embed_dir(
  TARGET luna_demo
  SOURCE_DIR "${CMAKE_SOURCE_DIR}/lua"
  NAMESPACE file
  MODE "${LUNA_EMBED}"
  STUB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lsp_stub"
  ${LUNA_STRIP_DEBUG_ARG})
