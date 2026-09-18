#pragma once

#include "luakit/core/api.hpp"
#include "luakit/module.hpp"

extern "C" auto luaopen_tracker(luakit::core::State *L) -> int;

namespace mod {

inline auto tracker() -> luakit::Module {
  return {"tracker", luaopen_tracker};
}

}  // namespace mod
