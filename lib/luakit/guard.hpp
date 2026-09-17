#pragma once

#include "luakit/api.hpp"

#include <cstdio>
#include <exception>

namespace luakit {

static constexpr const char *k_guard_prefix = "[guard] ";

// A C++ exception unwinding through Lua's C frames is undefined behaviour, so
// every function handed to Lua goes through this. aux::error is called outside
// the catch block on purpose: raising from inside longjmps away with the
// exception still live.
template <lua::CFunction F>
auto guard(lua::State *L) noexcept -> int {
  char msg[256];
  bool failed = false;

  try {
    return F(L);
  } catch (const std::exception &e) {
    std::snprintf(msg, sizeof msg, "%s", e.what());
    failed = true;
  } catch (...) {
    std::snprintf(msg, sizeof msg, "unknown C++ exception");
    failed = true;
  }

  if (failed) return lua::aux::error(L, "%s%s", k_guard_prefix, msg);
  return 0;
}

}  // namespace luakit
