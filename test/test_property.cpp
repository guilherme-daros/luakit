// Properties: fields on a registered class, and the per-instance state a
// script can stash alongside them.
//
// `entity.hp = 50` is what a mod author expects to be able to write, and the
// three things __index has to choose between -- a method, a property, and
// whatever the script itself put there -- all meet in this file.

#include "check.hpp"

#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/property.hpp"

#include <string>

namespace core = luakit::core;

namespace {

struct Entity {
  std::string name = "unnamed";
  int hp = 100;
  double speed = 1.0;

  auto heal(int n) -> Entity & {
    hp += n;
    return *this;
  }
  auto describe() const -> std::string { return name + "@" + std::to_string(hp); }

  // Backing a computed field: speed is stored, but capped on the way in.
  auto get_speed() const -> double { return speed; }
  auto set_speed(double v) -> void { speed = v > 10.0 ? 10.0 : v; }

  auto alive() const -> bool { return hp > 0; }
};

}  // namespace

template <>
struct luakit::Metatable<Entity> {
  static constexpr const char *k_name = "test.Entity";
};

namespace {

const core::aux::Reg entity_methods[] = {
    {"heal",     luakit::method<&Entity::heal>    },
    {"describe", luakit::method<&Entity::describe>},
    {nullptr,    nullptr                          },
};

const luakit::PropertyReg entity_fields[] = {
    luakit::prop<&Entity::hp>("hp"),
    luakit::ro_prop<&Entity::name>("name"),
    luakit::accessor<&Entity::get_speed, &Entity::set_speed>("speed"),
    luakit::accessor<&Entity::alive>("alive"),
    luakit::prop_end,
};

const core::aux::Reg mod[] = {
    {"new",   luakit::ctor<Entity>},
    {nullptr, nullptr             },
};

auto open_mod(core::State *L) -> int {
  core::aux::newlib(L, mod);
  return 1;
}

struct Host {
  luakit::Interpreter lua;
  Host() {
    lua.open_libs().bind<Entity>(entity_methods, nullptr, entity_fields).preload({"m", open_mod});
    lua.script("m = require('m')");
  }
};

auto test_read_and_write() -> void {
  t::section("a data member reads and writes as a field");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    assert(e.hp == 100)
    e.hp = 50
    assert(e.hp == 50)
    e.hp = e.hp - 10
    assert(e.hp == 40)
  )LUA"));
}

auto test_methods_still_work() -> void {
  t::section("methods and properties share one __index");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    assert(e:heal(5).hp == 105)
    assert(e:describe() == "unnamed@105")
    assert(type(e.heal) == "function")
  )LUA"));
}

auto test_read_only() -> void {
  t::section("a read-only field refuses assignment");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    assert(e.name == "unnamed")

    local ok, err = pcall(function() e.name = "renamed" end)
    assert(not ok)
    assert(err:find("read%-only"), err)
    assert(err:find("name"), err)

    -- and it really did not take
    assert(e.name == "unnamed")
  )LUA"));
}

auto test_accessor() -> void {
  t::section("a computed field goes through its methods");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    assert(e.speed == 1.0)

    e.speed = 4.5
    assert(e.speed == 4.5)

    -- the setter caps it, which is the point of having one
    e.speed = 1000
    assert(e.speed == 10.0)

    -- read-only computed
    assert(e.alive == true)
    local ok = pcall(function() e.alive = false end)
    assert(not ok)
  )LUA"));
}

auto test_field_type_is_checked() -> void {
  t::section("a field assignment is checked like an argument");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    local ok, err = pcall(function() e.hp = "lots" end)
    assert(not ok)
    assert(err:find("bad argument"), err)
    assert(e.hp == 100)

    local ok2 = pcall(function() e.speed = {} end)
    assert(not ok2)
  )LUA"));
}

// The uservalue slot: a mod stashing its own data on someone else's object.
auto test_per_instance_state() -> void {
  t::section("a script can stash its own fields on an object");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    assert(e.my_flag == nil)

    e.my_flag = true
    e.my_table = {1, 2, 3}
    e.counter = 0

    assert(e.my_flag == true)
    assert(#e.my_table == 3)

    e.counter = e.counter + 1
    assert(e.counter == 1)

    -- and it does not disturb the real fields
    assert(e.hp == 100)
    assert(e.name == "unnamed")
  )LUA"));
}

auto test_per_instance_state_is_per_instance() -> void {
  t::section("stashed state belongs to one object");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local a, b = m.new(), m.new()
    a.tag = "first"
    assert(a.tag == "first")
    assert(b.tag == nil)

    b.tag = "second"
    assert(a.tag == "first")
    assert(b.tag == "second")
  )LUA"));
}

// A field the script stashed survives collection along with the object.
auto test_stashed_state_survives_collection() -> void {
  t::section("stashed state survives a collection cycle");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    kept = m.new()
    kept.payload = { deep = "value" }
  )LUA"));

  for (int i = 0; i < 20; ++i) h.lua.script("collectgarbage('collect')");

  CHECK_OK(h.lua.script("assert(kept.payload.deep == 'value')"));
}

auto test_unknown_field_is_nil() -> void {
  t::section("an unknown field reads as nil, not an error");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local e = m.new()
    assert(e.nothing_like_this == nil)
    assert(rawequal(e.also_missing, nil))
  )LUA"));
}

// A getter that raises must surface as a Lua error from the field access.
auto test_getter_errors_propagate() -> void {
  t::section("a field access on a dead object still reports it");
  Host h;
  core::State *L = h.lua.raw();

  Entity local;
  luakit::Userdata<Entity>::push_ref(L, &local);
  core::setglobal(L, "borrowed");

  CHECK_OK(h.lua.script("assert(borrowed.hp == 100)"));

  luakit::Userdata<Entity>::invalidate(L, &local);
  CHECK_OK(h.lua.script(R"LUA(
    local ok, err = pcall(function() return borrowed.hp end)
    assert(not ok)
    assert(err:find("used after finalization"), err)
  )LUA"));
}

}  // namespace

auto main() -> int {
  test_read_and_write();
  test_methods_still_work();
  test_read_only();
  test_accessor();
  test_field_type_is_checked();
  test_per_instance_state();
  test_per_instance_state_is_per_instance();
  test_stashed_state_survives_collection();
  test_unknown_field_is_nil();
  test_getter_errors_propagate();
  return t::summary();
}
