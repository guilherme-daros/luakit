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

  # Matches the Makefile's -std=c++17. Turning extensions off is also what
  # makes CMake actually emit the flag: GCC already defaults to gnu++17, so
  # without this the standard never reaches compile_commands.json and clangd
  # falls back to whatever its own default happens to be.
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# luakit is self-contained and brings its own install/export rules.
add_subdirectory(luakit)

# Anything pulling luna in as a subproject wants the library, not the demo.
if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

option(LUNA_BUILD_DEMO "Build the demo host" ON)
option(LUNA_BUILD_TESTS "Build the luakit test suite" ON)
option(LUNA_SANITIZE "Build tests with AddressSanitizer and UBSan" ON)

if(LUNA_BUILD_DEMO)
  add_subdirectory(src)
endif()

if(LUNA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
