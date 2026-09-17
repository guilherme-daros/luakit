# Locates Lua 5.4 and provides the imported target Lua54::Lua54.
#
# CMake ships FindLua, but it happily matches any 5.x and reports the version
# only after the fact. luakit is 5.4-only (lua_newuserdatauv, and a
# static_assert on LUA_VERSION_NUM), so this looks for 5.4 specifically.
#
# pkg-config is used as a hint when present, but is not required.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_Lua54 QUIET lua5.4 lua-5.4 lua54)
endif()

find_path(
  Lua54_INCLUDE_DIR
  NAMES lua.h
  HINTS ${PC_Lua54_INCLUDE_DIRS}
  PATH_SUFFIXES lua5.4 lua-5.4 lua54 lua)

find_library(
  Lua54_LIBRARY
  NAMES lua5.4 lua-5.4 lua54 lua
  HINTS ${PC_Lua54_LIBRARY_DIRS})

# Confirm it really is 5.4 rather than whatever lua.h turned up first.
if(Lua54_INCLUDE_DIR AND EXISTS "${Lua54_INCLUDE_DIR}/lua.h")
  file(STRINGS "${Lua54_INCLUDE_DIR}/lua.h" _luakit_ver
       REGEX "^#define[ \t]+LUA_VERSION_NUM[ \t]+[0-9]+")
  string(REGEX MATCH "[0-9]+" Lua54_VERSION_NUM "${_luakit_ver}")
  if(Lua54_VERSION_NUM)
    math(EXPR _major "${Lua54_VERSION_NUM} / 100")
    math(EXPR _minor "${Lua54_VERSION_NUM} % 100")
    set(Lua54_VERSION "${_major}.${_minor}")
  endif()
  unset(_luakit_ver)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Lua54
  REQUIRED_VARS Lua54_LIBRARY Lua54_INCLUDE_DIR
  VERSION_VAR Lua54_VERSION)

if(Lua54_FOUND AND NOT TARGET Lua54::Lua54)
  add_library(Lua54::Lua54 UNKNOWN IMPORTED)
  set_target_properties(
    Lua54::Lua54
    PROPERTIES IMPORTED_LOCATION "${Lua54_LIBRARY}"
               INTERFACE_INCLUDE_DIRECTORIES "${Lua54_INCLUDE_DIR}")
  # Lua's math library calls need libm on most Unix systems.
  if(UNIX AND NOT APPLE)
    set_property(
      TARGET Lua54::Lua54
      APPEND
      PROPERTY INTERFACE_LINK_LIBRARIES m)
  endif()
endif()

mark_as_advanced(Lua54_INCLUDE_DIR Lua54_LIBRARY)
