#include "demo.hpp"

#include "file_scripts.hpp"

#include "luakit/callback.hpp"
#include "luakit/coroutine.hpp"

#include "packages/luna.hpp"
#include "packages/tracker.hpp"
#include "packages/world/world.hpp"

#include <cstdio>

namespace core = luakit::core;

namespace {

auto banner(const char *what) -> void {
  std::printf("\n=== %s ===\n", what);
}

auto open_examples(core::State *L) -> int {
  return luakit::load_script_as_module(L, luakit::file::script("lua/examples.lua"));
}

struct WorldSession {
  ~WorldSession() { world::shutdown(); }
};

}  // namespace

namespace demo {

auto run_frames(int count) -> void {
  banner("engine loop");
  std::printf("[C++] %d tick handler(s) registered\n", world::handler_count());
  for (int i = 0; i < count; ++i) world::tick(0.5);
}

auto run_scripted_task(luakit::Interpreter &lua) -> void {
  banner("scripted task");

  auto quest = luakit::Coroutine(lua, "quest");
  int turn = 0;
  while (auto turns = quest.resume<int>(turn)) {
    std::printf("[C++] the task asked to wait %d turn(s)\n", *turns);
    turn += *turns;
  }
  std::printf("[C++] the task finished after %d turn(s)\n", turn);
}

auto destroy_something_lua_holds(luakit::Interpreter &lua) -> void {
  banner("an object the engine takes back");
  std::printf("[C++] destroying 'goblin' while a script still holds it\n");
  world::destroy("goblin");
  lua.global<luakit::Function>("report_stale_handle").call<void>();
}

auto release_shared(luakit::Interpreter &lua) -> void {
  banner("shared ownership");
  std::printf("[C++] dropping the engine's reference to the summoned creature\n");
  world::release_summoned();
  lua.script("collectgarbage('collect')");
  lua.global<luakit::Function>("report_summoned").call<void>();
}

auto reload_plugin(luakit::Interpreter &lua) -> void {
  banner("hot reload");

  std::printf("[C++] world has been opened %d time(s)\n", world::open_count());

  lua.reload("world");

  std::printf("[C++] after reload, %d time(s)\n", world::open_count());
  std::printf("[C++] the population survived it: %d\n", world::population());
}

auto run(luakit::Interpreter &interpreter) -> void {
  auto session = WorldSession();

  interpreter.open_libs();

  interpreter.preload(mod::luna());
  interpreter.preload(mod::tracker());
  interpreter.preload(mod::world());

  interpreter.preload({"examples", open_examples});

  interpreter.load(luakit::file::script("lua/init.lua"));

  demo::run_frames(3);
  demo::run_scripted_task(interpreter);
  demo::destroy_something_lua_holds(interpreter);
  demo::release_shared(interpreter);
  demo::reload_plugin(interpreter);
}

}  // namespace demo
