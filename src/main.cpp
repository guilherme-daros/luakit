#include <cstdio>
#include <stdexcept>

#include "luakit/interpreter.hpp"

#include "init_lua.h"
#include "packages/luna.hpp"
#include "packages/tracker.hpp"

auto main(int argc, char **argv) -> int try {
  (void)argc;
  (void)argv;

  auto interpreter = luakit::Interpreter();

  // preload must follow open_libs, which is what creates `package`. Nothing
  // is loaded until init.lua requires it.
  interpreter.open_libs();
  interpreter.preload("luna", luaopen_luna);
  interpreter.preload("tracker", luaopen_tracker);

  interpreter.script_file("lua/init.lua");

  return 0;

} catch (const std::exception &e) {
  std::fprintf(stderr, "fatal: %s\n", e.what());
  return 1;
}
