#pragma once

#include "luakit/core/api.hpp"
#include "luakit/module.hpp"

extern "C" auto luaopen_luna(luakit::core::State *L) -> int;

namespace mod {

inline auto luna() -> luakit::Module {
  return {"luna", luaopen_luna};
}

}  // namespace mod
