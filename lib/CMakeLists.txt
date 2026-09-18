# luakit -- header-only C++20 bindings for Lua 5.4.
#
# Self-contained: this directory can be dropped into another project and used
# with add_subdirectory(luakit), pulled in with FetchContent, or installed and
# found with find_package(luakit).

cmake_minimum_required(VERSION 3.21)

project(
  luakit
  VERSION 0.1.0
  DESCRIPTION "Modern C++ bindings for Lua 5.4"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Must precede target_include_directories: CMAKE_INSTALL_INCLUDEDIR is
# undefined until this runs, and an empty INSTALL_INTERFACE silently exports a
# target with no usable include path.
include(GNUInstallDirs)

find_package(Lua54 REQUIRED)

add_library(luakit INTERFACE)
add_library(luakit::luakit ALIAS luakit)

target_compile_features(luakit INTERFACE cxx_std_20)
target_link_libraries(luakit INTERFACE Lua54::Lua54)

# The include root is this directory, so headers are reached as
# <luakit/guard.hpp>. Installed, they land in <prefix>/include/luakit/.
target_include_directories(
  luakit INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
                   "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# A consumer embedding luakit usually does not want its install rules.
option(LUAKIT_INSTALL "Generate luakit install and export rules"
       ${PROJECT_IS_TOP_LEVEL})

if(NOT LUAKIT_INSTALL)
  return()
endif()

include(CMakePackageConfigHelpers)

set(LUAKIT_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/luakit")

install(TARGETS luakit EXPORT luakitTargets)

install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/luakit"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING
  PATTERN "*.hpp")

install(
  EXPORT luakitTargets
  FILE luakit-targets.cmake
  NAMESPACE luakit::
  DESTINATION "${LUAKIT_CMAKE_DIR}")

# Shipped so the generated config can resolve Lua on the consumer's machine.
install(FILES "${CMAKE_CURRENT_LIST_DIR}/cmake/FindLua54.cmake"
        DESTINATION "${LUAKIT_CMAKE_DIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/luakit-config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/luakit-config.cmake"
  INSTALL_DESTINATION "${LUAKIT_CMAKE_DIR}")

# 0.x has no stable major yet, so require a matching minor.
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/luakit-config-version.cmake"
  COMPATIBILITY SameMinorVersion)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/luakit-config.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/luakit-config-version.cmake"
        DESTINATION "${LUAKIT_CMAKE_DIR}")
