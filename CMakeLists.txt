cmake_minimum_required(VERSION 3.21)

project(
  luna
  VERSION 0.1.0
  DESCRIPTION "Demo host embedding Lua 5.4 through luakit"
  LANGUAGES CXX)

# This repo is luakit's development home, so installing from here should
# install the library. An app that merely embeds luakit gets no install rules.
if(PROJECT_IS_TOP_LEVEL)
  set(LUAKIT_INSTALL ON)

  # compile_commands.json, for clangd and friends. Only when we are the top
  # project: forcing this on a parent build would be rude.
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

  # Turning extensions off is what makes CMake actually emit the standard:
  # GCC already defaults to gnu++17, so without this the flag never reaches
  # compile_commands.json and clangd falls back to its own default.
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# luakit is self-contained and brings its own install/export rules.
add_subdirectory(lib)

# Anything pulling luna in as a subproject wants the library, not the demo.
if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

option(LUNA_BUILD_DEMO "Build the demo host" ON)
option(LUNA_BUILD_TESTS "Build the luakit test suite" ON)
option(LUNA_SANITIZE "Build tests with AddressSanitizer and UBSan" ON)

# How lua/init.lua reaches the interpreter.
#
# bytecode: precompile with luac and bake the bytes into the executable.
# source:   bake the .lua text in and let liblua parse it at startup; slower
#           to start, but immune to bytecode version skew across machines.
# file:     read lua/init.lua from disk at startup, so editing a script means
#           rerunning rather than rebuilding.
#
# A release build has to be self-contained, so it embeds; a debug build
# favours the edit-run loop. An empty CMAKE_BUILD_TYPE is an unoptimised
# build, so it counts as a debug one. Multi-config generators cannot answer
# this at configure time and need -DLUNA_EMBED= to be passed explicitly.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
  set(LUNA_EMBED_DEFAULT "file")
else()
  set(LUNA_EMBED_DEFAULT "bytecode")
endif()

set(LUNA_EMBED "${LUNA_EMBED_DEFAULT}"
    CACHE STRING "How lua/init.lua reaches the interpreter")
set_property(CACHE LUNA_EMBED PROPERTY STRINGS bytecode source file)

# Strips line numbers and local names from the bytecode, which costs script
# errors their tracebacks. Off for a debug build, on the same reasoning as
# LUNA_EMBED: it only matters if bytecode was asked for explicitly there.
if(LUNA_EMBED_DEFAULT STREQUAL "file")
  set(LUNA_STRIP_DEBUG_DEFAULT OFF)
else()
  set(LUNA_STRIP_DEBUG_DEFAULT ON)
endif()

option(LUNA_STRIP_DEBUG "Strip debug info from embedded bytecode"
       ${LUNA_STRIP_DEBUG_DEFAULT})

if(LUNA_BUILD_DEMO)
  add_subdirectory(src)

  add_custom_target(
    run
    COMMAND luna_demo
    DEPENDS luna_demo
    USES_TERMINAL
    COMMENT "Running the demo host")
endif()

if(LUNA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

# ---------------------------------------------------------------- developer
# Tooling targets. Each is skipped with a note rather than failing configure,
# so a missing formatter never blocks a build.

# Regenerates the mechanical half of the lua:: layer. Only needed when the
# Lua version changes.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_custom_target(
    api
    COMMAND "${Python3_EXECUTABLE}" tools/gen_api.py --lua-include
            "${Lua54_INCLUDE_DIR}" -o lib/luakit/core/api_gen.hpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Regenerating lib/luakit/core/api_gen.hpp"
    VERBATIM)
else()
  message(STATUS "python3 not found: `api` target unavailable")
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
find_program(STYLUA_EXECUTABLE NAMES stylua)

# api_gen.hpp is generated, and excluded so the `api` target's output never
# fights the formatter.
file(GLOB_RECURSE LUNA_FORMAT_CXX CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/test/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/lib/luakit/*.hpp")
list(REMOVE_ITEM LUNA_FORMAT_CXX
     "${CMAKE_CURRENT_SOURCE_DIR}/lib/luakit/core/api_gen.hpp")

file(GLOB_RECURSE LUNA_FORMAT_LUA CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/lua/*.lua")

if(CLANG_FORMAT_EXECUTABLE AND STYLUA_EXECUTABLE)
  add_custom_target(
    format
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${LUNA_FORMAT_CXX}
    COMMAND "${STYLUA_EXECUTABLE}" ${LUNA_FORMAT_LUA}
    COMMENT "Formatting C++ (.clang-format) and Lua (stylua.toml)"
    VERBATIM)

  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/format-check.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/format-check.cmake" @ONLY)

  add_custom_target(
    format-check
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/format-check.cmake"
    COMMENT "Checking formatting"
    VERBATIM)
else()
  message(STATUS "clang-format or stylua not found: `format` targets unavailable")
endif()

# Editors look for the compilation database beside the sources, so link it
# there. Restricted to build directories inside the source tree: otherwise a
# throwaway build in /tmp silently repoints the editor at itself, and leaves a
# dangling link behind when it is deleted.
string(FIND "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/"
            LUNA_BUILD_IS_IN_TREE)
if(UNIX AND LUNA_BUILD_IS_IN_TREE EQUAL 0)
  file(CREATE_LINK "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
       "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
endif()
