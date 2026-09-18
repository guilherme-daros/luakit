// The container, enum and variadic conversions.
//
// These are the types a data-driven plugin actually passes around: a settings
// map, a coordinate pair, a named direction, a log call with whatever
// arguments the author felt like.

#include "check.hpp"

#include "luakit/enums.hpp"
#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/variadic.hpp"

#include <array>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core = luakit::core;

namespace {

enum class Facing { north, south, east, west };

}  // namespace

template <>
struct luakit::EnumNames<Facing> {
  static constexpr luakit::EnumEntry<Facing> k_values[] = {
      {"north", Facing::north},
      {"south", Facing::south},
      {"east",  Facing::east },
      {"west",  Facing::west },
  };
};

namespace {

using Settings = std::map<std::string, int>;

auto count_entries(Settings s) -> int {
  return static_cast<int>(s.size());
}

auto bump_all(Settings s, int by) -> Settings {
  for (auto &[k, v] : s) v += by;
  return s;
}

auto tally(std::unordered_map<std::string, double> m) -> double {
  double total = 0;
  for (const auto &[k, v] : m) total += v;
  return total;
}

auto swap_pair(std::pair<int, std::string> p) -> std::pair<std::string, int> {
  return {p.second, p.first};
}

auto sum3(std::array<double, 3> a) -> double {
  return a[0] + a[1] + a[2];
}

auto unit() -> std::array<double, 3> {
  return {1, 0, 0};
}

// A tuple as a *value* is a table; only a tuple return is multiple values.
auto describe(std::tuple<int, std::string, bool> t) -> std::string {
  return std::to_string(std::get<0>(t)) + "/" + std::get<1>(t) + "/" + (std::get<2>(t) ? "y" : "n");
}

auto pack() -> std::vector<std::pair<std::string, int>> {
  return {
      {"a", 1},
      {"b", 2}
  };
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

// The variadic case: a log line assembled from whatever was passed.
std::string last_log;

auto log_line(std::string level, luakit::Variadic rest) -> int {
  last_log = "[" + level + "]";
  for (int i = 0; i < rest.size(); ++i) last_log += " " + rest.to_string(i);
  return rest.size();
}

auto sum_all(luakit::Variadic args) -> double {
  double total = 0;
  for (int i = 0; i < args.size(); ++i) total += args.get_or<double>(i, 0);
  return total;
}

auto strict_sum(luakit::Variadic args) -> double {
  double total = 0;
  for (int i = 0; i < args.size(); ++i) total += args.get<double>(i);  // raises on a bad one
  return total;
}

const core::aux::Reg funcs[] = {
    {"count_entries", luakit::fn<count_entries>},
    {"bump_all",      luakit::fn<bump_all>     },
    {"tally",         luakit::fn<tally>        },
    {"swap_pair",     luakit::fn<swap_pair>    },
    {"sum3",          luakit::fn<sum3>         },
    {"unit",          luakit::fn<unit>         },
    {"describe",      luakit::fn<describe>     },
    {"pack",          luakit::fn<pack>         },
    {"opposite",      luakit::fn<opposite>     },
    {"log_line",      luakit::fn<log_line>     },
    {"sum_all",       luakit::fn<sum_all>      },
    {"strict_sum",    luakit::fn<strict_sum>   },
    {nullptr,         nullptr                  },
};

auto open_m(core::State *L) -> int {
  core::aux::newlib(L, funcs);
  return 1;
}

struct Host {
  luakit::Interpreter lua;
  Host() {
    lua.open_libs().preload({"m", open_m});
    lua.script("m = require('m')");
  }
};

auto test_maps() -> void {
  t::section("maps round-trip through tables");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    assert(m.count_entries{a = 1, b = 2, c = 3} == 3)
    assert(m.count_entries{} == 0)

    local out = m.bump_all({x = 1, y = 2}, 10)
    assert(out.x == 11 and out.y == 12)

    assert(m.tally{a = 1.5, b = 2.5} == 4.0)
  )LUA"));

  // A number key read as a string would convert the slot in place and break
  // the traversal, so this is the case the key copy exists for.
  Settings s;
  for (int i = 0; i < 50; ++i) s.emplace("k" + std::to_string(i), i);
  h.lua.set_global("many", s);
  CHECK_EQ(h.lua.global<Settings>("many").size(), s.size());
  CHECK_EQ(h.lua.global<Settings>("many").at("k7"), 7);

  CHECK_OK(h.lua.script("numeric = {[1] = 10, [2] = 20, [3] = 30}"));
  auto numeric = h.lua.global<std::map<int, int>>("numeric");
  CHECK_EQ(static_cast<int>(numeric.size()), 3);
  CHECK_EQ(numeric.at(2), 20);
}

auto test_map_diagnostics() -> void {
  t::section("a bad map entry says which key");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    local ok, err = pcall(m.count_entries, {a = 1, b = "not a number"})
    assert(not ok)
    assert(err:find("bad argument #1"), err)
    assert(err:find("'b'"), err)

    local ok2, err2 = pcall(m.count_entries, 7)
    assert(not ok2)
    assert(err2:find("bad argument #1"), err2)
  )LUA"));
}

auto test_pair_and_array() -> void {
  t::section("pair and array are fixed-length sequences");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    local p = m.swap_pair{7, "seven"}
    assert(p[1] == "seven" and p[2] == 7)

    assert(m.sum3{1, 2, 3} == 6)

    local u = m.unit()
    assert(#u == 3 and u[1] == 1 and u[2] == 0)

    -- The length is part of the type.
    local ok, err = pcall(m.sum3, {1, 2})
    assert(not ok)
    assert(err:find("expected 3 elements, got 2"), err)

    local ok2, err2 = pcall(m.sum3, {1, 2, 3, 4})
    assert(not ok2)
    assert(err2:find("expected 3 elements, got 4"), err2)

    local ok3, err3 = pcall(m.sum3, {1, "x", 3})
    assert(not ok3)
    assert(err3:find("element 2"), err3)
  )LUA"));
}

auto test_tuple_as_a_value() -> void {
  t::section("a tuple parameter is a table");
  Host h;
  CHECK_OK(h.lua.script(R"LUA(
    assert(m.describe{3, "mid", true} == "3/mid/y")

    -- Nested inside a container, too.
    local packed = m.pack()
    assert(#packed == 2)
    assert(packed[1][1] == "a" and packed[1][2] == 1)
    assert(packed[2][1] == "b" and packed[2][2] == 2)
  )LUA"));
}

auto test_enums() -> void {
  t::section("enums are strings on the Lua side");
  Host h;

  CHECK_OK(h.lua.script(R"LUA(
    assert(m.opposite("north") == "south")
    assert(m.opposite("east") == "west")
  )LUA"));

  // A misspelling is caught, and the message lists the alternatives.
  CHECK_OK(h.lua.script(R"LUA(
    local ok, err = pcall(m.opposite, "nrth")
    assert(not ok)
    assert(err:find("bad argument #1"), err)
    assert(err:find("'north'"), err)
    assert(err:find("'west'"), err)
  )LUA"));

  // A raw ordinal is not accepted; that is the whole point of naming them.
  CHECK_OK(h.lua.script(R"LUA(
    local ok, err = pcall(m.opposite, 0)
    assert(not ok, "a number should not pass as an enum")
  )LUA"));

  h.lua.set_global("facing", Facing::west);
  CHECK_STR(h.lua.global<std::string>("facing"), "west");
  CHECK(h.lua.global<Facing>("facing") == Facing::west);
}

auto test_variadic() -> void {
  t::section("a Variadic takes whatever is left");
  Host h;

  CHECK_OK(h.lua.script("assert(m.log_line('warn', 'disk', 42, true) == 3)"));
  CHECK_STR(last_log, "[warn] disk 42 true");

  CHECK_OK(h.lua.script("assert(m.log_line('info') == 0)"));
  CHECK_STR(last_log, "[info]");

  CHECK_OK(h.lua.script(R"LUA(
    assert(m.sum_all() == 0)
    assert(m.sum_all(1, 2, 3) == 6)
    assert(m.sum_all(1, "skipped", 3) == 4)  -- get_or ignores what will not convert
  )LUA"));

  // get() raises, naming the offending argument by its real position.
  CHECK_OK(h.lua.script(R"LUA(
    local ok, err = pcall(m.strict_sum, 1, {}, 3)
    assert(not ok)
    assert(err:find("bad argument #2"), err)
  )LUA"));
}

auto test_variadic_stack_traits() -> void {
  t::section("Variadic borrows, so it cannot escape the call");
  static_assert(luakit::Stack<luakit::Variadic>::borrows);
  static_assert(!luakit::Stack<std::map<std::string, int>>::borrows);
  static_assert(!luakit::Stack<std::pair<int, int>>::borrows);
  static_assert(luakit::Stack<std::pair<int, std::string_view>>::borrows);
  static_assert(!luakit::Stack<std::array<double, 3>>::borrows);
  static_assert(!luakit::Stack<Facing>::borrows);
}

auto test_round_trip_through_globals() -> void {
  t::section("containers survive a trip through a global");
  Host h;

  const std::vector<std::pair<std::string, int>> v{
      {"one", 1},
      {"two", 2}
  };
  h.lua.set_global("pairs_list", v);
  const auto back = h.lua.global<std::vector<std::pair<std::string, int>>>("pairs_list");
  CHECK_EQ(static_cast<int>(back.size()), 2);
  CHECK_STR(back[1].first, "two");
  CHECK_EQ(back[1].second, 2);

  const std::array<int, 4> a{1, 2, 3, 4};
  h.lua.set_global("arr", a);
  CHECK_EQ((h.lua.global<std::array<int, 4>>("arr")[3]), 4);

  const std::unordered_map<std::string, std::string> um{
      {"k", "v"}
  };
  h.lua.set_global("um", um);
  CHECK_STR((h.lua.global<std::unordered_map<std::string, std::string>>("um").at("k")), "v");
}

}  // namespace

auto main() -> int {
  test_maps();
  test_map_diagnostics();
  test_pair_and_array();
  test_tuple_as_a_value();
  test_enums();
  test_variadic();
  test_variadic_stack_traits();
  test_round_trip_through_globals();
  return t::summary();
}
