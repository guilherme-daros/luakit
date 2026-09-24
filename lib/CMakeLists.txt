# luakit -- header-only C++20 bindings for Lua 5.4.
#
# Self-contained: this directory can be dropped into another project and used
# with add_subdirectory(luakit), pulled in with FetchContent, or installed and
# found with find_package(luakit).

cmake_minimum_required(VERSION 3.21)

project(
  luakit
  VERSION 0.2.0
  DESCRIPTION "Modern C++ bindings for Lua 5.4"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# luakit_embed_script(), for hosts baking their scripts into the executable.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/luakit-embed.cmake")

# Must precede target_include_directories: CMAKE_INSTALL_INCLUDEDIR is
# undefined until this runs, and an empty INSTALL_INTERFACE silently exports a
# target with no usable include path.
include(GNUInstallDirs)

find_package(Lua54 REQUIRED)

add_library(luakit INTERFACE)
add_library(luakit::luakit ALIAS luakit)

target_compile_features(luakit INTERFACE cxx_std_20)
target_link_libraries(luakit INTERFACE Lua54::Lua54)

# The warning set this repo builds its own targets with. Not INTERFACE on
# luakit itself: a consumer chooses their own warnings, and inheriting ours
# through a link would be presumptuous.
add_library(luakit_warnings INTERFACE)
add_library(luakit::warnings ALIAS luakit_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    luakit_warnings
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wnon-virtual-dtor
              -Wcast-align
              -Wunused
              -Woverloaded-virtual
              -Wdouble-promotion)
elseif(MSVC)
  target_compile_options(luakit_warnings INTERFACE /W4)
endif()

# The include root is this directory, so headers are reached as
# <luakit/guard.hpp>. Installed, they land in <prefix>/include/luakit/.
target_include_directories(
  luakit INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
                   "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# "Each header stands on its own, so you include what you name" is a claim the
# README makes, and exactly the kind that rots without anyone noticing: the
# first file to include a header usually included something else first.
#
# So compile one translation unit per public header, containing nothing but
# that include. A missing include then fails the build rather than waiting to
# surprise whoever reaches for the header on its own.
option(LUAKIT_BUILD_CHECKS "Build the per-header self-containment check" OFF)

if(LUAKIT_BUILD_CHECKS)
  # core/api_gen.hpp is deliberately excluded: it is private to core/api.hpp
  # and says so.
  file(GLOB LUAKIT_PUBLIC_HEADERS CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/luakit/*.hpp"
       "${CMAKE_CURRENT_SOURCE_DIR}/luakit/core/api.hpp")

  set(luakit_self_contained_srcs "")
  foreach(header IN LISTS LUAKIT_PUBLIC_HEADERS)
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}" "${header}")
    string(MAKE_C_IDENTIFIER "${rel}" stem)

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/self-contained/${stem}.cpp")
    file(WRITE "${generated}" "// Generated: proves <${rel}> needs nothing before it.\n#include \"${rel}\"\n")
    list(APPEND luakit_self_contained_srcs "${generated}")
  endforeach()

  add_library(luakit_self_contained OBJECT ${luakit_self_contained_srcs})
  target_link_libraries(luakit_self_contained PRIVATE luakit::luakit luakit::warnings)
endif()

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

# FindLua54 so the generated config can resolve Lua on the consumer's machine,
# luakit-embed so an installed luakit still offers luakit_embed_script/_dir,
# and embedded_stub.hpp.in alongside it -- installed, it is no longer a
# sibling of lib/cmake/ the way it is in-tree, since headers move under
# <prefix>/include instead of staying under <prefix>/lib/luakit.
install(FILES "${CMAKE_CURRENT_LIST_DIR}/cmake/FindLua54.cmake"
              "${CMAKE_CURRENT_LIST_DIR}/cmake/luakit-embed.cmake"
              "${CMAKE_CURRENT_LIST_DIR}/luakit/embedded_stub.hpp.in"
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
