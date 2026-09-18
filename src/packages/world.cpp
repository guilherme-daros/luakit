// A tiny game world, and the whole point of the library in one package.
//
// The engine owns its creatures and merely lends them to scripts; a script
// registers handlers the engine fires; both sides pass tables, maps, enums and
// values back and forth. Everything here is what a mod would actually touch.
//
// luna.cpp and tracker.cpp show the same binding with hand-written Reg arrays.
// This one uses the Class<T> builder, which is the same registration with the
// arrays folded away.

#include "world.hpp"

#include "luakit/callback.hpp"
#include "luakit/class.hpp"
#include "luakit/coroutine.hpp"
#include "luakit/enums.hpp"
#include "luakit/function.hpp"
#include "luakit/overload.hpp"
#include "luakit/property.hpp"
#include "luakit/table.hpp"
#include "luakit/userdata.hpp"
#include "luakit/variadic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace core = luakit::core;

namespace world {

// ---------------------------------------------------------------- types

// A value type. Lua gets its own copy, which the collector owns, because a
// vector with no home in the engine has nothing to borrow from.
struct Vec2 {
  double x = 0;
  double y = 0;

  Vec2() = default;
  Vec2(double a, double b) : x(a), y(b) {}

  auto length() const -> double { return std::sqrt(x * x + y * y); }
  auto plus(Vec2 other) const -> Vec2 { return {x + other.x, y + other.y}; }
  auto plus(double scalar) const -> Vec2 { return {x + scalar, y + scalar}; }
  auto describe() const -> std::string {
    char buf[48];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f)", x, y);
    return buf;
  }
};

enum class Facing { north, south, east, west };

// The engine's own objects. Scripts see pointers to these and never own one.
class Entity {
 public:
  std::string name;
  Vec2 position;

  explicit Entity(std::string n) : name(std::move(n)) {}
  virtual ~Entity() = default;

  // Lua holds these by address, so copying one would give a script a handle to
  // something the engine does not know about.
  Entity(const Entity &) = delete;
  auto operator=(const Entity &) -> Entity & = delete;

  auto move_by(Vec2 delta) -> Entity & {
    position = position.plus(delta);
    return *this;
  }

  auto describe() const -> std::string { return "<" + name + " at " + position.describe() + ">"; }
};

// Single inheritance here because that is what a world like this needs.
// Several bases work the same way, with the pointer adjusted for each.
class Creature : public Entity {
 public:
  using Entity::Entity;

  auto damage(int amount) -> Creature & {
    hp_ = std::max(0, hp_ - amount);
    return *this;
  }

  auto health() const -> int { return hp_; }

  // A setter is what makes this worth being an accessor rather than a plain
  // field: the clamp applies however a script writes to it.
  auto set_health(int v) -> void { hp_ = std::clamp(v, 0, 100); }

  auto alive() const -> bool { return hp_ > 0; }

  auto facing() const -> Facing { return facing_; }
  auto set_facing(Facing f) -> void { facing_ = f; }

 private:
  int hp_ = 100;
  Facing facing_ = Facing::north;
};

}  // namespace world

// A specialization has to name the primary template's namespace, so these
// cannot live inside namespace world.
template <>
struct luakit::Metatable<world::Vec2> {
  static constexpr const char *k_name = "world.Vec2";
};
template <>
struct luakit::Metatable<world::Entity> {
  static constexpr const char *k_name = "world.Entity";
};
template <>
struct luakit::Metatable<world::Creature> {
  static constexpr const char *k_name = "world.Creature";
  using bases = luakit::Bases<world::Entity>;
};

// An enum reaches Lua as a string, so a script writes `c.facing = "north"` and
// a misspelling is caught at the assignment with the alternatives listed.
template <>
struct luakit::EnumNames<world::Facing> {
  static constexpr luakit::EnumEntry<world::Facing> k_values[] = {
      {"north", world::Facing::north},
      {"south", world::Facing::south},
      {"east",  world::Facing::east },
      {"west",  world::Facing::west },
  };
};

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

const core::aux::Reg funcs[] = {
    {"spawn", luakit::fn<spawn>},
    {"find", luakit::fn<find>},
    {"summon", luakit::fn<summon>},
    {"banish", luakit::fn<banish>},
    {"census", luakit::fn<census>},
    {"spawn_many", luakit::fn<spawn_many>},
    {"opposite", luakit::fn<opposite>},
    {"configure", luakit::fn<configure>},
    {"log", luakit::fn<log>},
    {"on_tick", luakit::fn<on_tick>},
    {"wait", luakit::yielding<wait>},
    {"distance", luakit::fn_overload<static_cast<double (*)(Vec2, Vec2)>(distance),
     static_cast<double (*)(Creature *, Creature *)>(distance)>},
    {"vec2", luakit::ctor_overload<Vec2, luakit::Args<>, luakit::Args<double, double>>},
    {nullptr, nullptr},
};

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

extern "C" auto luaopen_world(core::State *L) -> int {
  using namespace world;

  ++opens;
  host_state = L;

  // Registration order matters: a base has to be in place before the classes
  // that inherit from it copy its members down.
  luakit::Class<Vec2>(L)
      .method<&Vec2::length>("length")
      .overload<static_cast<Vec2 (Vec2::*)(Vec2) const>(&Vec2::plus),
                static_cast<Vec2 (Vec2::*)(double) const>(&Vec2::plus)>("plus")
      .prop<&Vec2::x>("x")
      .prop<&Vec2::y>("y")
      .meta<&Vec2::describe>("__tostring")
      .build();

  luakit::Class<Entity>(L)
      .method<&Entity::move_by>("move_by")
      .ro_prop<&Entity::name>("name")
      .prop<&Entity::position>("position")
      .meta<&Entity::describe>("__tostring")
      .build();

  luakit::Class<Creature>(L)
      .method<&Creature::damage>("damage")
      .accessor<&Creature::health, &Creature::set_health>("hp")
      .accessor<&Creature::alive>("alive")
      .accessor<&Creature::facing, &Creature::set_facing>("facing")
      .build();

  core::aux::newlib(L, funcs);
  return 1;
}
