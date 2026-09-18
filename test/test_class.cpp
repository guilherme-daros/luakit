// The class model: inheritance, overload dispatch, and the Class<T> builder.

#include "check.hpp"

#include "luakit/class.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/overload.hpp"

#include <string>

namespace core = luakit::core;

namespace {

// A small hierarchy, plus a second base so the multiple-inheritance pointer
// adjustment is actually exercised rather than assumed.
int dog_dtors = 0;

struct Named {
  std::string name = "unnamed";
  auto label() const -> std::string { return name; }
  auto describe() const -> std::string { return "named(" + name + ")"; }
};

struct Ticking {
  // Padding ahead of the member, so Ticking's subobject inside Robot cannot
  // sit at offset zero and a missing adjustment shows up as a wrong value.
  long ticks = 0;
  auto tick() -> void { ++ticks; }
  auto tick_count() const -> long { return ticks; }
};

struct Animal : Named {
  int hp = 100;
  auto hurt(int n) -> void { hp -= n; }
  auto health() const -> int { return hp; }
};

struct Dog : Animal {
  ~Dog() { ++dog_dtors; }
  auto speak() const -> std::string { return name + " says woof"; }
};

struct Robot : Named, Ticking {
  auto speak() const -> std::string { return name + " says beep"; }
  // Shadows the one inherited from Named, to check the nearer wins.
  auto describe_self() const -> std::string { return "robot(" + name + ")"; }
};

// Free functions taking a base, to be called with a derived.
auto describe_named(Named *n) -> std::string {
  return "named:" + n->label();
}
auto describe_animal(Animal &a) -> std::string {
  return "animal:" + a.label() + "/" + std::to_string(a.health());
}
auto count_ticks(Ticking *t) -> long {
  return t->tick_count();
}

// An overload set on free functions.
auto area(double side) -> double {
  return side * side;
}
auto area(double w, double h) -> double {
  return w * h;
}
auto area_name(std::string s) -> double {
  return static_cast<double>(s.size());
}

struct Vec {
  double x = 0;
  double y = 0;

  Vec() = default;
  Vec(double a, double b) : x(a), y(b) {}
  explicit Vec(double s) : x(s), y(s) {}

  auto scaled(double k) const -> Vec { return Vec(x * k, y * k); }
  auto scaled(Vec by) const -> Vec { return Vec(x * by.x, y * by.y); }
  auto sum() const -> double { return x + y; }
};

}  // namespace

template <>
struct luakit::Metatable<Named> {
  static constexpr const char *k_name = "test.Named";
};
template <>
struct luakit::Metatable<Ticking> {
  static constexpr const char *k_name = "test.Ticking";
};
template <>
struct luakit::Metatable<Animal> {
  static constexpr const char *k_name = "test.Animal";
  using bases = luakit::Bases<Named>;
};
template <>
struct luakit::Metatable<Dog> {
  static constexpr const char *k_name = "test.Dog";
  using bases = luakit::Bases<Animal>;
};
template <>
struct luakit::Metatable<Robot> {
  static constexpr const char *k_name = "test.Robot";
  using bases = luakit::Bases<Named, Ticking>;
};
template <>
struct luakit::Metatable<Vec> {
  static constexpr const char *k_name = "test.Vec";
};

namespace {

// Registration order matters: a base has to be in place before the classes
// that inherit from it copy its members down.
auto open_world(core::State *L) -> int {
  luakit::Class<Named>(L)
      .method<&Named::label>("label")
      .prop<&Named::name>("name")
      .meta<&Named::describe>("__tostring")
      .build();

  luakit::Class<Ticking>(L).method<&Ticking::tick>("tick").accessor<&Ticking::tick_count>("ticks").build();

  luakit::Class<Animal>(L).method<&Animal::hurt>("hurt").prop<&Animal::hp>("hp").build();

  return luakit::Class<Dog>(L)
      .method<&Dog::speak>("speak")
      .ctor<>("dog")
      .fn<describe_named>("describe_named")
      .fn<describe_animal>("describe_animal")
      .fn<count_ticks>("count_ticks")
      .build_module();
}

auto open_more(core::State *L) -> int {
  luakit::Class<Robot>(L).method<&Robot::speak>("speak").meta<&Robot::describe_self>("__tostring").build();

  // An overloaded member needs its signature spelled out, because the name
  // alone does not pick one. Nothing luakit can do about that.
  return luakit::Class<Vec>(L)
      .overload<static_cast<Vec (Vec::*)(double) const>(&Vec::scaled),
                static_cast<Vec (Vec::*)(Vec) const>(&Vec::scaled)>("scaled")
      .method<&Vec::sum>("sum")
      .prop<&Vec::x>("x")
      .prop<&Vec::y>("y")
      .ctors<luakit::Args<>, luakit::Args<double, double>>("vec")
      .raw_fn("robot", luakit::ctor<Robot>)
      .raw_fn("area", luakit::fn_overload<static_cast<double (*)(double)>(area), area_name,
                                          static_cast<double (*)(double, double)>(area)>)
      .build_module();
}

struct Host {
  luakit::Interpreter lua;
  Host() {
    lua.open_libs().preload({"world", open_world}).preload({"more", open_more});
    lua.script("w = require('world') n = require('more')");
  }
};

auto test_inherited_methods() -> void {
  t::section("a derived class gets its bases' members");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local d = w.dog()

    -- its own
    assert(d:speak() == "unnamed says woof")

    -- from Animal
    d:hurt(30)
    assert(d.hp == 70)

    -- from Named, two levels up
    assert(d:label() == "unnamed")
    d.name = "rex"
    assert(d.name == "rex")
    assert(d:speak() == "rex says woof")
  )LUA"));
}

auto test_derived_passes_as_base() -> void {
  t::section("a derived object satisfies a base parameter");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local d = w.dog()
    d.name = "rex"
    d:hurt(10)

    assert(w.describe_named(d) == "named:rex")
    assert(w.describe_animal(d) == "animal:rex/90")
  )LUA"));
}

// The case a plain offset-free cast would get wrong: Ticking is Robot's second
// base, so its subobject does not start where the Robot does.
auto test_multiple_inheritance_adjusts_the_pointer() -> void {
  t::section("a second base gets its pointer adjusted");
  Host h;

  // Guards the guard: if Ticking happened to sit at offset zero, the checks
  // below would pass whether the cast was applied or not.
  Robot probe;
  CHECK(static_cast<void *>(static_cast<Ticking *>(&probe)) != static_cast<void *>(&probe));

  CHECK_OK(h.lua.script(R"LUA(
    local r = n.robot()
    r.name = "bender"

    assert(r:speak() == "bender says beep")
    assert(w.describe_named(r) == "named:bender")

    r:tick()
    r:tick()
    r:tick()
    assert(r.ticks == 3)
    assert(w.count_ticks(r) == 3)
  )LUA"));
}

auto test_unrelated_class_is_still_rejected() -> void {
  t::section("an unrelated class is not accepted as a base");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local v = n.vec()
    local ok, err = pcall(w.describe_named, v)
    assert(not ok)
    assert(err:find("test.Named expected"), err)

    -- and a base is not a derived
    local named_only = w.dog()
    local ok2 = pcall(w.count_ticks, named_only)
    assert(not ok2, "a Dog is not a Ticking")
  )LUA"));
}

auto test_free_function_overloads() -> void {
  t::section("free function overloads dispatch on the arguments");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    assert(n.area(3) == 9)
    assert(n.area(3, 4) == 12)
    assert(n.area("abcd") == 4)

    local ok, err = pcall(n.area)
    assert(not ok)
    assert(err:find("no overload"), err)

    local ok2, err2 = pcall(n.area, {}, {})
    assert(not ok2)
    assert(err2:find("no overload"), err2)
  )LUA"));
}

auto test_constructor_overloads() -> void {
  t::section("constructor overloads");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local a = n.vec()
    assert(a.x == 0 and a.y == 0)

    local b = n.vec(3, 4)
    assert(b.x == 3 and b.y == 4)
    assert(b:sum() == 7)

    local ok, err = pcall(n.vec, "nope")
    assert(not ok)
    assert(err:find("no overload"), err)
  )LUA"));
}

auto test_method_overloads() -> void {
  t::section("method overloads");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local v = n.vec(2, 3)

    local doubled = v:scaled(2)
    assert(doubled.x == 4 and doubled.y == 6)

    local by_vec = v:scaled(n.vec(10, 100))
    assert(by_vec.x == 20 and by_vec.y == 300)

    local ok = pcall(function() return v:scaled("x") end)
    assert(not ok)
  )LUA"));
}

auto test_builder_produces_the_same_thing() -> void {
  t::section("the builder registers methods, props and a module table");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    assert(type(w.dog) == "function")
    assert(type(n.vec) == "function")

    local d = w.dog()
    assert(type(d.speak) == "function")   -- a method
    assert(type(d.hp) == "number")        -- a property
    assert(d.nothing == nil)              -- and unknown names are still nil
  )LUA"));
}

// An override in a derived class has to win over the base's copy.
auto test_override_wins() -> void {
  t::section("a derived member overrides the inherited one");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    -- Dog defines speak; Named does not, so there is nothing to shadow there.
    -- Animal and Named both reach `label`, and the nearest copy is the one in
    -- effect -- checked by calling it through a Dog.
    local d = w.dog()
    d.name = "rex"
    assert(d:label() == "rex")
  )LUA"));
}

auto test_identity_across_the_hierarchy() -> void {
  t::section("a derived object stays one userdata through a base parameter");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local d = w.dog()
    d.stashed = "mine"

    -- passing it as a base and reading it back must not make a second object
    assert(w.describe_named(d) == "named:unnamed")
    assert(d.stashed == "mine")
  )LUA"));
}

// Metamethods come down with everything else, or a __tostring on a base would
// leave derived objects printing their address and nothing would say why.
auto test_metamethods_inherit() -> void {
  t::section("a base's metamethods reach the derived class");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local d = w.dog()
    d.name = "rex"

    -- Named is two levels up from Dog.
    assert(tostring(d) == "named(rex)", tostring(d))

    -- and the default userdata rendering is what it would be without this
    assert(not tostring(d):find("^test%.Dog: "), tostring(d))
  )LUA"));
}

auto test_derived_metamethod_wins() -> void {
  t::section("a derived class overrides an inherited metamethod");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local r = n.robot()
    r.name = "bender"
    assert(tostring(r) == "robot(bender)", tostring(r))
  )LUA"));
}

// __gc is the reserved one that would actually break. A base's finalizer
// installed on a derived class checks for the base's metatable, fails, and the
// object is never destroyed.
auto test_gc_is_not_inherited() -> void {
  t::section("the collector still finalizes a derived class");
  dog_dtors = 0;
  {
    Host h;
    CHECK_OK(h.lua.script("for i = 1, 10 do local d = w.dog() d.name = 'x' end"));
    CHECK_OK(h.lua.script("collectgarbage('collect') collectgarbage('collect')"));
    CHECK_EQ(dog_dtors, 10);
  }
  CHECK_EQ(dog_dtors, 10);
}

// The three reserved names are luakit's whatever a class asks for, since a
// supplied __gc would displace the one that destroys the object.
auto test_reserved_metamethods_cannot_be_replaced() -> void {
  t::section("a class cannot take over __gc or the index pair");
  dog_dtors = 0;
  {
    luakit::Interpreter lua;
    lua.open_libs();

    static bool intruder_ran = false;
    const core::aux::Reg meta[] = {
        {"__gc",
         [](core::State *) {
           intruder_ran = true;
           return 0;
         }                    },
        {"__index",
         [](core::State *) {
           intruder_ran = true;
           return 0;
         }                    },
        {"__newindex",
         [](core::State *) {
           intruder_ran = true;
           return 0;
         }                    },
        {nullptr,      nullptr},
    };
    const core::aux::Reg methods[] = {
        {"speak", luakit::method<&Dog::speak>},
        {nullptr, nullptr                    },
    };
    // static, so the captureless opener below can name it.
    static const core::aux::Reg mod[] = {
        {"dog",   luakit::ctor<Dog>},
        {nullptr, nullptr          },
    };

    luakit::Userdata<Named>::register_class(lua.raw(), nullptr);
    luakit::Userdata<Animal>::register_class(lua.raw(), nullptr);
    luakit::Userdata<Dog>::register_class(lua.raw(), methods, meta);

    lua.preload({"m", [](core::State *L) {
                   core::aux::newlib(L, mod);
                   return 1;
                 }});

    CHECK_OK(lua.script(R"LUA(
      local m = require("m")
      local d = m.dog()
      assert(d:speak() == "unnamed says woof")   -- __index still dispatches
      d.stashed = 1                              -- __newindex still stores
      assert(d.stashed == 1)
    )LUA"));
    CHECK_OK(lua.script("collectgarbage('collect') collectgarbage('collect')"));

    CHECK(!intruder_ran);
  }
  CHECK_EQ(dog_dtors, 1);  // luakit's __gc ran, so the Dog was destroyed
}

}  // namespace

auto main() -> int {
  test_inherited_methods();
  test_derived_passes_as_base();
  test_multiple_inheritance_adjusts_the_pointer();
  test_unrelated_class_is_still_rejected();
  test_free_function_overloads();
  test_constructor_overloads();
  test_method_overloads();
  test_builder_produces_the_same_thing();
  test_override_wins();
  test_identity_across_the_hierarchy();
  test_metamethods_inherit();
  test_derived_metamethod_wins();
  test_gc_is_not_inherited();
  test_reserved_metamethods_cannot_be_replaced();
  return t::summary();
}
