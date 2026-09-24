// The LuaLS definitions, generated from the registration itself.
//
// The point of the feature is that a mod author's editor knows what the host
// exposes, so the checks here are about the annotations carrying the
// information only the C++ side had: the class hierarchy, which fields are
// read-only, and the exact strings an enum accepts.

#include "check.hpp"

#include "luakit/class.hpp"
#include "luakit/doc.hpp"
#include "luakit/enums.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/library.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

enum class Mood { calm, angry };

struct Thing {
  std::string label;
  int size = 1;
  auto grow(int by) -> Thing & {
    size += by;
    return *this;
  }
  auto mood() const -> Mood { return Mood::calm; }
  auto set_mood(Mood) -> void {}
};

struct Gadget : Thing {
  auto beep() const -> std::string { return "beep"; }
};

auto tally(std::vector<std::string> names) -> std::map<std::string, int> {
  std::map<std::string, int> out;
  for (auto &n : names) out[n] = 1;
  return out;
}
auto maybe(std::optional<int> n) -> int {
  return n.value_or(0);
}
auto pick_one(int a) -> int {
  return a;
}
auto pick_two(int a, std::string b) -> std::string {
  return b + std::to_string(a);
}

}  // namespace

template <>
struct luakit::Metatable<Thing> {
  static constexpr const char *k_name = "kit.Thing";
};
template <>
struct luakit::Metatable<Gadget> {
  static constexpr const char *k_name = "kit.Gadget";
  using bases = luakit::Bases<Thing>;
};
template <>
struct luakit::EnumNames<Mood> {
  static constexpr luakit::EnumEntry<Mood> k_values[] = {
      {"calm",  Mood::calm },
      {"angry", Mood::angry},
  };
};

namespace {

auto open_kit(luakit::core::State *L) -> int {
  luakit::Class<Thing>(L, "kit")
      .method<&Thing::grow>("grow")
      .prop<&Thing::size>("size")
      .ro_prop<&Thing::label>("label")
      .accessor<&Thing::mood, &Thing::set_mood>("mood")
      .ctor<>("new")
      .build();

  luakit::Class<Gadget>(L, "kit").method<&Gadget::beep>("beep").build();

  return luakit::Library(L, "kit")
      .fn<tally>("tally")
      .fn<maybe>("maybe")
      .overload<pick_one, pick_two>("pick")
      .enum_<Mood>("Mood")
      .build_module();
}

auto has(const std::string &text, const char *needle) -> bool {
  return text.find(needle) != std::string::npos;
}

}  // namespace

auto main() -> int {
  luakit::Interpreter lua;
  lua.openlibs().preload({"kit", open_kit});
  lua.script("require('kit')");

  const std::string defs = luakit::doc::emit_module("kit");

  t::section("the file is a LuaLS meta file for the module");
  CHECK(has(defs, "---@meta kit"));
  CHECK(has(defs, "return kit"));

  t::section("classes carry their name and their bases");
  CHECK(has(defs, "---@class kit.Thing"));
  CHECK(has(defs, "---@class kit.Gadget : kit.Thing"));

  t::section("fields carry their type, and read-only says so");
  CHECK(has(defs, "---@field size integer"));
  CHECK(has(defs, "---@field label string  # read-only"));

  t::section("an enum field is the literal union, not just 'string'");
  CHECK(has(defs, R"(---@field mood "calm"|"angry")"));

  t::section("and the enum's names are reachable as a table");
  CHECK(has(defs, R"(---@type table<string, "calm"|"angry">)"));
  CHECK(has(defs, "kit.Mood = {}"));
  CHECK_STR(lua.eval<std::string>("return require('kit').Mood.angry"), "angry");

  t::section("methods carry parameter and return types");
  CHECK(has(defs, "---@param a1 integer"));
  CHECK(has(defs, "---@return kit.Thing"));
  CHECK(has(defs, "function Thing:grow(a1) end"));
  CHECK(has(defs, "function Gadget:beep() end"));

  t::section("containers and optionals become their LuaLS spellings");
  CHECK(has(defs, "---@param a1 string[]"));
  CHECK(has(defs, "---@return table<string, integer>"));
  CHECK(has(defs, "---@param a1 integer?"));

  t::section("an overload set lists its other arms");
  CHECK(has(defs, "---@overload fun(a1: integer, a2: string): string"));

  t::section("a constructor lands in the module, not on the class");
  CHECK(has(defs, "function kit.new() end"));

  t::section("registering again replaces rather than duplicates");
  const std::size_t before = luakit::doc::registry().classes.size();
  lua.reload("kit");
  CHECK_EQ(luakit::doc::registry().classes.size(), before);
  CHECK_STR(luakit::doc::emit_module("kit"), defs);

  t::section("the module is listed");
  bool listed = false;
  for (const auto &name : luakit::doc::module_names()) listed = listed || name == "kit";
  CHECK(listed);

  return t::summary();
}
