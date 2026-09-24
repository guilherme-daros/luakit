// Handing a base pointer out for an object that is really derived.
//
// Passing a Derived into a Base parameter is test_class.cpp's job. This is the
// other direction, which used to produce two unrelated Lua objects for one C++
// object: `==` false, the derived members missing, and per-instance state
// invisible from whichever handle did not set it.

#include "check.hpp"

#include "luakit/class.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/library.hpp"

#include <string>

namespace {

struct Animal {
  virtual ~Animal() = default;
  std::string name = "animal";
  auto speak() const -> std::string { return "..."; }
};

struct Dog : Animal {
  int legs = 4;
  auto fetch() const -> std::string { return "fetches"; }
};

// Registered, but never declares Animal as a base. Resolving a pushed Animal *
// to this would leave the value unreadable as an Animal *, so it must not be.
struct Stray : Animal {
  int fleas = 7;
};

// Deliberately never registered, to check the fallback.
struct Ghost : Animal {};

Dog g_dog;
Stray g_stray;
Ghost g_ghost;

auto as_animal() -> Animal * {
  return &g_dog;
}
auto as_dog() -> Dog * {
  return &g_dog;
}
auto stray_as_animal() -> Animal * {
  return &g_stray;
}
auto ghost_as_animal() -> Animal * {
  return &g_ghost;
}
auto animal_name(Animal *a) -> std::string {
  return a->name;
}

}  // namespace

template <>
struct luakit::Metatable<Animal> {
  static constexpr const char *k_name = "test.Animal";
};
template <>
struct luakit::Metatable<Dog> {
  static constexpr const char *k_name = "test.Dog";
  using bases = luakit::Bases<Animal>;
};
template <>
struct luakit::Metatable<Stray> {
  static constexpr const char *k_name = "test.Stray";
};

namespace {

auto open_zoo(luakit::core::State *L) -> int {
  luakit::Class<Animal>(L).method<&Animal::speak>("speak").prop<&Animal::name>("name").build();
  luakit::Class<Dog>(L).method<&Dog::fetch>("fetch").prop<&Dog::legs>("legs").build();
  luakit::Class<Stray>(L).prop<&Stray::fleas>("fleas").build();

  return luakit::Library(L)
      .fn<as_animal>("as_animal")
      .fn<as_dog>("as_dog")
      .fn<stray_as_animal>("stray_as_animal")
      .fn<ghost_as_animal>("ghost_as_animal")
      .fn<animal_name>("animal_name")
      .build_module();
}

}  // namespace

auto main() -> int {
  luakit::Interpreter lua;
  lua.openlibs().preload({"zoo", open_zoo});
  lua.script("z = require('zoo')");

#if LUAKIT_HAS_RTTI
  t::section("a base pointer arrives as its most derived registered type");
  CHECK_EQ(lua.eval<bool>("return z.as_animal() == z.as_dog()"), true);
  CHECK_EQ(lua.eval<int>("return z.as_animal().legs"), 4);
  CHECK_STR(lua.eval<std::string>("return z.as_animal():fetch()"), "fetches");

  t::section("one object, one set of per-instance state");
  lua.script("z.as_animal().tag = 'set through the base'");
  CHECK_STR(lua.eval<std::string>("return z.as_dog().tag"), "set through the base");
#else
  // Resolution needs typeid and dynamic_cast. Without RTTI a base pointer
  // stays a base pointer, which is what it did before the feature existed --
  // and is still correct, just less useful.
  t::section("without RTTI, a base pointer stays the static type");
  CHECK_EQ(lua.eval<bool>("return z.as_animal() ~= z.as_dog()"), true);
  CHECK_EQ(lua.eval<bool>("return z.as_animal().legs == nil"), true);
#endif

  t::section("the base's own members still work through it");
  CHECK_STR(lua.eval<std::string>("return z.as_animal():speak()"), "...");
  CHECK_STR(lua.eval<std::string>("return z.as_animal().name"), "animal");

  t::section("and it is still accepted where a base is expected");
  CHECK_STR(lua.eval<std::string>("return z.animal_name(z.as_animal())"), "animal");
  CHECK_STR(lua.eval<std::string>("return z.animal_name(z.as_dog())"), "animal");

  t::section("a derived class that never declared its base is not resolved to");
  // Stray is registered, so the typeid lookup finds it -- but it cannot be
  // read back as an Animal *, so pushing it as one has to stay as one.
  CHECK_STR(lua.eval<std::string>("return z.animal_name(z.stray_as_animal())"), "animal");
  CHECK_EQ(lua.eval<bool>("return z.stray_as_animal().fleas == nil"), true);

  t::section("an unregistered derived class falls back to the static type");
  CHECK_STR(lua.eval<std::string>("return z.animal_name(z.ghost_as_animal())"), "animal");

  t::section("the identity cache is keyed on the object, not the spelling");
#if LUAKIT_HAS_RTTI
  CHECK_EQ(lua.eval<bool>("return z.as_dog() == z.as_animal()"), true);
#endif
  CHECK_EQ(lua.eval<bool>("return z.as_animal() ~= z.stray_as_animal()"), true);
  CHECK_EQ(lua.eval<bool>("return z.as_dog() == z.as_dog()"), true);

  return t::summary();
}
