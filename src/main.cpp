#include <cstdio>

#include "luakit/interpreter.hpp"

#include "packages/luna.hpp"
#include "packages/tracker.hpp"

#ifdef LUNA_EMBEDDED_SCRIPT
#include "init_lua.h"  // generated: init_lua[], init_lua_len
#endif

auto main(int argc, char **argv) -> int try {
  (void)argc;
  (void)argv;

  auto interpreter = luakit::Interpreter();

  interpreter.open_libs();
  interpreter.preload(mod::luna());
  interpreter.preload(mod::tracker());

#ifdef LUNA_EMBEDDED_SCRIPT
  interpreter.script_bytecode(init_lua, init_lua_len, "@lua/init.lua");
#else
  interpreter.script_file(LUNA_SCRIPT_PATH);
#endif

  return 0;

} catch (const std::exception &e) {
  std::fprintf(stderr, "fatal: %s\n", e.what());
  return 1;
}
