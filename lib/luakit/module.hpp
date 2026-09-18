// A Lua module: the name it is required by, paired with the opener that
// builds it. This is what Interpreter::preload registers.
//
// Its own header, so a package can describe itself by including this and
// nothing else. Pulling in interpreter.hpp for the sake of one struct would
// drag Stack<T> and Userdata<T> along with it.

#pragma once

#include "luakit/core/api.hpp"

namespace luakit {

struct Module {
  const char *name;
  core::CFunction open;
};

}  // namespace luakit
