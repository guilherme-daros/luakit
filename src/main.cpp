#include <cstdio>
#include <stdexcept>

#include "luakit/state.hpp"

#include "init_lua.h"
#include "luna.hpp"
#include "tracker.hpp"

auto main(int argc, char **argv) -> int try {
  (void)argc;
  (void)argv;

  luakit::State state;

  // preload must follow open_libs, which is what creates `package`. Nothing
  // is loaded until init.lua requires it.
  state.open_libs().preload("luna", luaopen_luna).preload("tracker", luaopen_tracker);

  state.script_bytecode(init_lua, init_lua_len, "@lua/init.lua");

  return 0;

} catch (const std::exception &e) {
  std::fprintf(stderr, "fatal: %s\n", e.what());
  return 1;
}
