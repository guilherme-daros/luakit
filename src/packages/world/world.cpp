// A tiny game world, and the whole point of the library in one package.
//
// The engine owns its creatures and merely lends them to scripts; a script
// registers handlers the engine fires; both sides pass tables, maps, enums and
// values back and forth. Everything here is what a mod would actually touch.
//
// Vec2, Entity and Creature each get their own file in this directory,
// registered with the Class<T> builder; the free functions below are
// registered with luakit::Library, its counterpart for a module with no
// class involved.

#include "world.hpp"

#include "creature.hpp"
#include "entity.hpp"
#include "vec2.hpp"

#include "luakit/callback.hpp"
#include "luakit/coroutine.hpp"
#include "luakit/library.hpp"
#include "luakit/overload.hpp"
#include "luakit/table.hpp"
#include "luakit/variadic.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace core = luakit::core;

namespace world {

// ---------------------------------------------------------------- state

namespace {

// Everything the engine owns. Nothing in here is reachable from Lua except
// through a borrowed pointer.
std::deque<std::unique_ptr<Creature>> creatures;
std::shared_ptr<Creature> summoned;
std::vector<luakit::Function> tick_handlers;

int opens = 0;
double clock_seconds = 0;

struct Settings {
  std::string title = "untitled";
  int difficulty = 1;
  bool verbose = false;
} settings;

// The state this package was opened on. invalidate() only touches the
// registry, which every thread of an interpreter shares, so any live State of
// it will do; require() happens on the main thread here.
core::State *host_state = nullptr;

auto find_slot(const std::string &name) -> std::deque<std::unique_ptr<Creature>>::iterator {
  return std::find_if(creatures.begin(), creatures.end(), [&](const auto &c) { return c->name == name; });
}

// ------------------------------------------------------------ bindings

// Returns a pointer into the engine's own storage. The userdata is marked
// borrowed, so collecting it leaves the creature alone.
auto spawn(std::string name) -> Creature * {
  creatures.push_back(std::make_unique<Creature>(std::move(name)));
  return creatures.back().get();
}

// A null pointer arrives as nil, so an absent creature reads as absent rather
// than as an object that faults on first use.
auto find(std::string name) -> Creature * {
  auto it = find_slot(name);
  return it == creatures.end() ? nullptr : it->get();
}

// Shared ownership: the engine and the script both hold a count, so neither
// has to outlive the other.
auto summon(std::string name) -> std::shared_ptr<Creature> {
  summoned = std::make_shared<Creature>(std::move(name));
  return summoned;
}

// The engine breaking its half of the borrowed deal, on purpose. invalidate
// severs the handle first, so a script touching it afterwards is told so
// instead of reading freed memory.
auto banish(std::string name) -> bool {
  auto it = find_slot(name);
  if (it == creatures.end()) return false;

  luakit::Userdata<Creature>::invalidate(host_state, it->get());
  creatures.erase(it);
  return true;
}

// A map goes across as a table, which is what a script wants to iterate.
auto census() -> std::map<std::string, int> {
  std::map<std::string, int> out;
  for (const auto &c : creatures) out.emplace(c->name, c->health());
  return out;
}

// A vector in, a pair out: the shapes a data-driven mod passes around.
auto spawn_many(std::vector<std::string> names) -> std::pair<int, int> {
  const int before = static_cast<int>(creatures.size());
  for (auto &n : names) spawn(std::move(n));
  return {before, static_cast<int>(creatures.size())};
}

auto opposite(Facing f) -> Facing {
  switch (f) {
    case Facing::north:
      return Facing::south;
    case Facing::south:
      return Facing::north;
    case Facing::east:
      return Facing::west;
    case Facing::west:
      return Facing::east;
  }
  return f;
}

// Two signatures under one Lua name, picked by what the call actually passes.
auto distance(Vec2 a, Vec2 b) -> double {
  return Vec2(a.x - b.x, a.y - b.y).length();
}
auto distance(Creature *a, Creature *b) -> double {
  return distance(a->position, b->position);
}

// A configuration table read straight off the script.
auto configure(luakit::Table cfg) -> void {
  settings.title = cfg.get_or<std::string>("title", settings.title);
  settings.difficulty = cfg.get_or<int>("difficulty", settings.difficulty);
  settings.verbose = cfg.get_or<bool>("verbose", settings.verbose);

  std::printf("[C++] configured: title=%s difficulty=%d verbose=%s\n", settings.title.c_str(), settings.difficulty,
              settings.verbose ? "true" : "false");
}

// Whatever the caller felt like passing, rendered for a human.
auto log(std::string level, luakit::Variadic rest) -> int {
  std::string line = "[C++] log[" + level + "]";
  for (int i = 0; i < rest.size(); ++i) line += " " + rest.to_string(i);
  std::printf("%s\n", line.c_str());
  return rest.size();
}

// A handler registered now and fired later, from the engine's own loop. This
// is the direction that makes a plugin a plugin.
auto on_tick(luakit::Function cb) -> void {
  tick_handlers.push_back(std::move(cb));
}

// Suspends the coroutine that called it. The engine decides when to resume,
// which is how a script writes `wait(2)` without owning the schedule.
auto wait(int turns) -> int {
  return turns;  // yielded to whoever is driving, so it knows how long to hold
}

}  // namespace

// ------------------------------------------------------------ host side

auto tick(double dt) -> void {
  clock_seconds += dt;
  for (const auto &handler : tick_handlers) {
    // One plugin's fault must not cost the others their frame. The call is
    // protected, so this catches rather than crashes.
    try {
      handler.call<void>(dt);
    } catch (const luakit::Error &e) {
      std::printf("[C++] a tick handler failed: %s\n", e.what());
    }
  }
}

auto handler_count() -> int {
  return static_cast<int>(tick_handlers.size());
}

auto open_count() -> int {
  return opens;
}

auto destroy(const char *name) -> bool {
  return banish(name);
}

auto release_summoned() -> void {
  summoned.reset();
}

auto population() -> int {
  return static_cast<int>(creatures.size());
}

auto shutdown() -> void {
  // The handlers are the part that matters: each one holds a registry
  // reference it will try to give back. The rest is tidiness.
  tick_handlers.clear();
  summoned.reset();
  creatures.clear();
  host_state = nullptr;
}

}  // namespace world

auto luaopen_world(core::State *L) -> int {
  using namespace world;

  ++opens;
  host_state = L;

  // Registration order matters: a base has to be in place before the classes
  // that inherit from it copy its members down.
  register_vec2(L);
  register_entity(L);
  register_creature(L);

  return luakit::Library(L, "world")
      .fn<spawn>("spawn")
      .fn<find>("find")
      .fn<summon>("summon")
      .fn<banish>("banish")
      .fn<census>("census")
      .fn<spawn_many>("spawn_many")
      .fn<opposite>("opposite")
      .enum_<Facing>("Facing")
      .fn<configure>("configure")
      .fn<world::log>("log")  // qualified: unqualified log is ambiguous with ::log(double)
      .fn<on_tick>("on_tick")
      .yielding_fn<wait>("wait")
      .overload<static_cast<double (*)(Vec2, Vec2)>(distance),
                static_cast<double (*)(Creature *, Creature *)>(distance)>("distance")
      .ctors<Vec2, luakit::Args<>, luakit::Args<double, double>>("vec2")
      .build_module();
}
