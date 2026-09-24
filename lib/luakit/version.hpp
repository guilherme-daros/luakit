// The library's version, for a consumer that found it with find_package and
// needs to compile against more than one of them.

#pragma once

#define LUAKIT_VERSION_MAJOR 0
#define LUAKIT_VERSION_MINOR 2
#define LUAKIT_VERSION_PATCH 0

// Comparable as a whole: #if LUAKIT_VERSION >= 200 is "0.2.0 or later".
#define LUAKIT_VERSION (LUAKIT_VERSION_MAJOR * 10000 + LUAKIT_VERSION_MINOR * 100 + LUAKIT_VERSION_PATCH)

#define LUAKIT_VERSION_STRING "0.2.0"

namespace luakit {

inline constexpr int version_major = LUAKIT_VERSION_MAJOR;
inline constexpr int version_minor = LUAKIT_VERSION_MINOR;
inline constexpr int version_patch = LUAKIT_VERSION_PATCH;
inline constexpr const char *version_string = LUAKIT_VERSION_STRING;

}  // namespace luakit
