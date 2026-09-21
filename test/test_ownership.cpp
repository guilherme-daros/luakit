// Ownership: who destroys the object behind a userdata.
//
// This is the case that makes luakit usable as a plugin host. A game owns its
// entities and merely lends them to a script; a script that builds its own
// objects gets them collected like anything else; a shared_ptr lets neither
// side have to outlive the other. All three arrive through one metatable, so
// every method has to work on all three without knowing which it has.
//
// Run under ASan/LSan: half of what is asserted here is "nothing was freed
// twice" and "nothing was leaked", which only a sanitizer can see.

#include "check.hpp"

#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace core = luakit::core;

namespace {

int entity_dtors = 0;

struct Entity {
  std::string name;
  int hp = 100;

  explicit Entity(std::string n) : name(std::move(n)) {}
  ~Entity() { ++entity_dtors; }

  Entity(const Entity &) = delete;
  auto operator=(const Entity &) -> Entity & = delete;

  auto damage(int n) -> Entity & {
    hp -= n;
    return *this;
  }
  auto health() const -> int { return hp; }
  auto label() const -> std::string { return name; }
};

int point_dtors = 0;

// Copyable, so it can go through Lua by value.
struct Point {
  double x = 0;
  double y = 0;

  Point() = default;
  Point(double a, double b) : x(a), y(b) {}
  ~Point() { ++point_dtors; }
  Point(const Point &) = default;
  auto operator=(const Point &) -> Point & = default;

  auto sum() const -> double { return x + y; }
};

}  // namespace

template <>
struct luakit::Metatable<Entity> {
  static constexpr const char *k_name = "test.Entity";
};
template <>
struct luakit::Metatable<Point> {
  static constexpr const char *k_name = "test.Point";
};

namespace {

// The engine's own storage. Scripts never own anything in here.
std::vector<std::unique_ptr<Entity>> world;
std::shared_ptr<Entity> host_shared;

auto spawn(std::string name) -> Entity * {
  world.push_back(std::make_unique<Entity>(std::move(name)));
  return world.back().get();
}

auto find(std::string name) -> Entity * {
  for (const auto &e : world) {
    if (e->name == name) return e.get();
  }
  return nullptr;  // pushes nil rather than a userdata that faults on use
}

auto share(std::string name) -> std::shared_ptr<Entity> {
  host_shared = std::make_shared<Entity>(std::move(name));
  return host_shared;
}

auto origin() -> Point {
  return Point(3, 4);
}

auto sum_of(Point p) -> double {
  return p.sum();
}

const core::aux::Reg entity_methods[] = {
    {"damage", luakit::method<&Entity::damage>},
    {"health", luakit::method<&Entity::health>},
    {"label",  luakit::method<&Entity::label> },
    {nullptr,  nullptr                        },
};

const core::aux::Reg point_methods[] = {
    {"sum",   luakit::method<&Point::sum>},
    {nullptr, nullptr                    },
};

const core::aux::Reg engine[] = {
    {"spawn", luakit::fn<spawn>},
    {"find", luakit::fn<find>},
    {"share", luakit::fn<share>},
    {"origin", luakit::fn<origin>},
    {"sum_of", luakit::fn<sum_of>},
    {"point", luakit::ctor<Point, double, double>},
    {nullptr, nullptr},
};

auto open_engine(core::State *L) -> int {
  core::aux::newlib(L, engine);
  return 1;
}

struct Host {
  luakit::Interpreter lua;
  Host() {
    world.clear();
    host_shared.reset();
    entity_dtors = 0;
    point_dtors = 0;
    lua.openlibs().bind<Entity>(entity_methods).bind<Point>(point_methods).preload({"engine", open_engine});
  }
  ~Host() {
    world.clear();
    host_shared.reset();
  }
};

// The headline case: Lua uses an object the host owns, then forgets it, and
// the host's object is untouched.
auto test_borrowed_is_not_collected() -> void {
  t::section("a borrowed object survives collection");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    local e = engine.spawn("goblin")
    assert(e:label() == "goblin")
    assert(e:damage(30):health() == 70)
    e = nil
  )LUA"));

  for (int i = 0; i < 20; ++i) h.lua.script("collectgarbage('collect')");

  CHECK_EQ(entity_dtors, 0);  // __gc left it alone
  CHECK_EQ(static_cast<int>(world.size()), 1);
  CHECK_EQ(world[0]->hp, 70);  // and Lua really did mutate it
}

// Two pushes of one object give one userdata, so equality works and a field a
// script sets on it is still there next time the host hands it over.
auto test_identity_is_preserved() -> void {
  t::section("pushing one object twice gives one userdata");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    engine.spawn("orc")
    local a = engine.find("orc")
    local b = engine.find("orc")

    assert(rawequal(a, b), "the same object should be the same userdata")
    assert(a == b)

    a:damage(25)
    assert(b:health() == 75, b:health())
  )LUA"));

  CHECK_EQ(world[0]->hp, 75);
}

// Distinct objects must not collide, however the cache is keyed.
auto test_distinct_objects_stay_distinct() -> void {
  t::section("different objects get different userdata");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    engine.spawn("a")
    engine.spawn("b")
    assert(not rawequal(engine.find("a"), engine.find("b")))
    assert(engine.find("a"):label() == "a")
    assert(engine.find("b"):label() == "b")
  )LUA"));
}

// An owned object reached by pointer has to resolve to the userdata that
// already owns it, or the collector would end up with two boxes for one T.
auto test_owned_object_is_found_by_pointer() -> void {
  t::section("a pointer into an owned box finds that box");
  Host h;
  core::State *L = h.lua.raw();

  luakit::push(L, Point(1, 2));
  Point *inside = luakit::Stack<Point *>::get(L, -1);

  luakit::Stack<Point *>::push(L, inside);
  CHECK(core::rawequal(L, -1, -2));
  CHECK(luakit::Userdata<Point>::ownership(L, -1) == luakit::Ownership::owned);
  core::pop(L, 2);
}

// Values are weak, so the table must not keep dead userdata reachable.
auto test_cache_does_not_leak_entries() -> void {
  t::section("the cache lets collected userdata go");
  Host h;
  core::State *L = h.lua.raw();

  for (int i = 0; i < 500; ++i) {
    luakit::push(L, Point(i, i));
    core::pop(L, 1);
  }
  CHECK_OK(h.lua.script("collectgarbage('collect') collectgarbage('collect')"));

  // Count what survived. A strong-valued cache would hold all 500.
  luakit::Userdata<Point>::push_ref(L, nullptr);  // nil, but forces nothing
  core::pop(L, 1);

  CHECK_OK(h.lua.script(R"LUA(
    local n = 0
    for _, t in pairs(debug.getregistry()) do
      if type(t) == "table" and getmetatable(t) and getmetatable(t).__mode == "v" then
        for _ in pairs(t) do n = n + 1 end
      end
    end
    cache_entries = n
  )LUA"));

  core::getglobal(L, "cache_entries");
  const int live = luakit::get<int>(L, -1);
  core::pop(L, 1);
  CHECK(live < 50);  // a handful may survive the cycle; 500 would mean a leak
}

// The host breaking its half of the borrowed deal, handled deliberately.
auto test_invalidate_severs_a_borrow() -> void {
  t::section("invalidate turns a stale borrow into a clean error");
  Host h;
  core::State *L = h.lua.raw();

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    doomed = engine.spawn("doomed")
    assert(doomed:health() == 100)
  )LUA"));

  Entity *p = world[0].get();
  luakit::Userdata<Entity>::invalidate(L, p);
  world.clear();
  CHECK_EQ(entity_dtors, 1);

  // Touching it now says so, rather than reading freed memory.
  CHECK_OK(h.lua.script(R"LUA(
    local ok, err = pcall(function() return doomed:health() end)
    assert(not ok)
    assert(err:find("used after finalization"), err)
  )LUA"));

  // And the address is free to be reused without inheriting the dead box.
  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    local fresh = engine.spawn("fresh")
    assert(fresh:health() == 100)
    assert(not rawequal(fresh, doomed))
  )LUA"));
}

auto test_null_pushes_nil() -> void {
  t::section("a null pointer arrives as nil");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    assert(engine.find("nobody") == nil)
  )LUA"));
}

// A value return gives Lua its own copy, and the collector does destroy that.
auto test_owned_by_value() -> void {
  t::section("a value return is owned and collected");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    local p = engine.origin()
    assert(p:sum() == 7)
    -- and it goes back in by value too
    assert(engine.sum_of(p) == 7)
    assert(engine.sum_of(engine.point(1, 1)) == 2)
  )LUA"));

  const int before = point_dtors;
  CHECK_OK(h.lua.script("collectgarbage('collect') collectgarbage('collect')"));
  CHECK(point_dtors > before);  // the copies Lua held really were destroyed
}

auto test_shared_keeps_the_object_alive() -> void {
  t::section("a shared object outlives whichever side lets go first");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    kept = engine.share("wraith")
    assert(kept:label() == "wraith")
  )LUA"));

  CHECK_EQ(host_shared.use_count(), 2);  // the host's and Lua's

  // The host lets go; Lua's reference keeps it alive.
  host_shared.reset();
  for (int i = 0; i < 20; ++i) h.lua.script("collectgarbage('collect')");
  CHECK_EQ(entity_dtors, 0);

  CHECK_OK(h.lua.script("assert(kept:health() == 100)"));

  // Now Lua lets go too, and the last count goes with it.
  CHECK_OK(h.lua.script("kept = nil"));
  for (int i = 0; i < 20; ++i) h.lua.script("collectgarbage('collect')");
  CHECK_EQ(entity_dtors, 1);
}

auto test_shared_dropped_by_lua_first() -> void {
  t::section("Lua dropping a shared object leaves the host's count");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    local e = engine.share("lich")
    e = nil
  )LUA"));

  for (int i = 0; i < 20; ++i) h.lua.script("collectgarbage('collect')");

  CHECK_EQ(entity_dtors, 0);
  CHECK_EQ(host_shared.use_count(), 1);
  CHECK_STR(host_shared->name, "lich");
}

// Every method goes through Userdata<T>::check, which reads one pointer field
// whatever the box holds. Proving that means mixing the modes in one script.
auto test_all_modes_share_one_metatable() -> void {
  t::section("methods work the same on all three modes");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    local borrowed = engine.spawn("borrowed")
    local shared = engine.share("shared")

    assert(borrowed:label() == "borrowed")
    assert(shared:label() == "shared")
    assert(borrowed:damage(10):health() == 90)
    assert(shared:damage(20):health() == 80)
  )LUA"));

  CHECK_EQ(world[0]->hp, 90);
  CHECK_EQ(host_shared->hp, 80);
}

auto test_ownership_is_reported() -> void {
  t::section("ownership() names the mode");
  Host h;
  core::State *L = h.lua.raw();

  Entity local("stack-resident");
  luakit::Userdata<Entity>::push_ref(L, &local);
  CHECK(luakit::Userdata<Entity>::ownership(L, -1) == luakit::Ownership::borrowed);
  core::pop(L, 1);

  auto sp = std::make_shared<Entity>("counted");
  luakit::Userdata<Entity>::push_shared(L, sp);
  CHECK(luakit::Userdata<Entity>::ownership(L, -1) == luakit::Ownership::shared);
  core::pop(L, 1);

  luakit::push(L, Point(1, 2));
  CHECK(luakit::Userdata<Point>::ownership(L, -1) == luakit::Ownership::owned);
  core::pop(L, 1);

  CHECK_OK(h.lua.script("collectgarbage('collect')"));
  entity_dtors = 0;  // `local` and `sp` die at scope exit, which is not the point here
}

// A count can only come from a box that has one. Aliasing an owned or borrowed
// object into a fresh shared_ptr would invent a second owner for it.
auto test_shared_get_only_from_a_shared_box() -> void {
  t::section("shared_ptr is only recovered from a shared box");
  Host h;
  core::State *L = h.lua.raw();

  Entity local("borrowed-only");
  luakit::Userdata<Entity>::push_ref(L, &local);
  CHECK(luakit::Stack<std::shared_ptr<Entity>>::get(L, -1) == nullptr);
  core::pop(L, 1);

  auto sp = std::make_shared<Entity>("counted");
  luakit::Userdata<Entity>::push_shared(L, sp);
  auto back = luakit::Stack<std::shared_ptr<Entity>>::get(L, -1);
  CHECK(back != nullptr);
  CHECK(back.get() == sp.get());
  CHECK_EQ(sp.use_count(), 3);  // sp, the box, and `back`
  core::pop(L, 1);

  CHECK_OK(h.lua.script("collectgarbage('collect')"));
  entity_dtors = 0;
}

// A borrowed box must not be destroyed at interpreter close either, which is a
// separate path from a collection cycle.
auto test_close_does_not_free_borrowed() -> void {
  t::section("closing the interpreter leaves borrowed objects alone");
  world.clear();
  host_shared.reset();
  entity_dtors = 0;

  // Built by hand rather than through Host, whose destructor clears the world
  // and would take the survivor with it before it could be examined.
  {
    luakit::Interpreter lua;
    lua.openlibs().bind<Entity>(entity_methods).preload({"engine", open_engine});
    CHECK_OK(lua.script(R"LUA(
      local engine = require("engine")
      survivor = engine.spawn("survivor")
    )LUA"));
    CHECK_EQ(entity_dtors, 0);
  }  // lua_close finalizes everything reachable

  CHECK_EQ(entity_dtors, 0);
  CHECK_EQ(static_cast<int>(world.size()), 1);
  CHECK_STR(world[0]->name, "survivor");

  world.clear();
  CHECK_EQ(entity_dtors, 1);  // and the host's own teardown is what frees it
}

}  // namespace

auto main() -> int {
  test_borrowed_is_not_collected();
  test_identity_is_preserved();
  test_distinct_objects_stay_distinct();
  test_owned_object_is_found_by_pointer();
  test_cache_does_not_leak_entries();
  test_invalidate_severs_a_borrow();
  test_null_pushes_nil();
  test_owned_by_value();
  test_shared_keeps_the_object_alive();
  test_shared_dropped_by_lua_first();
  test_all_modes_share_one_metatable();
  test_ownership_is_reported();
  test_shared_get_only_from_a_shared_box();
  test_close_does_not_free_borrowed();
  return t::summary();
}
