// Table: reading and writing Lua tables from the host.
//
// The motivating case is a plugin's configuration table: the host wants some
// fields, will accept defaults for others, and must not fall over because a
// script author typed a string where a number belonged.

#include "check.hpp"

#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/table.hpp"

#include <string>
#include <utility>

namespace core = luakit::core;

namespace {

using luakit::Error;
using luakit::Interpreter;
using luakit::Table;

// What a plugin's setup call hands the host.
int seen_width = 0;
std::string seen_title;

auto configure(Table cfg) -> void {
  seen_width = cfg.get_or<int>("width", 0);
  seen_title = cfg.get_or<std::string>("title", "");
}

const core::aux::Reg engine[] = {
    {"configure", luakit::fn<configure>},
    {nullptr,     nullptr              },
};

auto open_engine(core::State *L) -> int {
  core::aux::newlib(L, engine);
  return 1;
}

struct Host {
  Interpreter lua;
  Host() {
    lua.openlibs();
    lua.script(R"LUA(
      config = {
        title = "demo",
        width = 1280,
        scale = 1.5,
        fullscreen = false,
        tags = {"a", "b", "c"},
        nested = { deep = { answer = 42 } },
      }
    )LUA");
  }
};

auto test_reading_fields() -> void {
  t::section("reading typed fields");
  Host h;
  Table cfg = h.lua.global<Table>("config");

  CHECK(cfg.valid());
  CHECK_STR(cfg.get<std::string>("title"), "demo");
  CHECK_EQ(cfg.get<int>("width"), 1280);
  CHECK(cfg.get<double>("scale") == 1.5);
  CHECK_EQ(cfg.get<bool>("fullscreen"), false);
}

auto test_missing_and_wrong_type() -> void {
  t::section("get throws, get_or falls back");
  Host h;
  Table cfg = h.lua.global<Table>("config");

  bool caught = false;
  try {
    cfg.get<int>("height");
  } catch (const Error &e) {
    caught = true;
    const std::string msg = e.what();
    CHECK(msg.find("'height'") != std::string::npos);  // names the field
    CHECK(msg.find("nil") != std::string::npos);
  }
  CHECK(caught);

  caught = false;
  try {
    cfg.get<int>("title");
  } catch (const Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("'title'") != std::string::npos);
  }
  CHECK(caught);

  // A setting takes the default rather than bringing the host down, whether
  // the field is missing or the author put the wrong thing there.
  CHECK_EQ(cfg.get_or<int>("height", 720), 720);
  CHECK_EQ(cfg.get_or<int>("title", 99), 99);
  CHECK_EQ(cfg.get_or<int>("width", 99), 1280);
}

auto test_has() -> void {
  t::section("has");
  Host h;
  Table cfg = h.lua.global<Table>("config");

  CHECK(cfg.has("title"));
  CHECK(!cfg.has("height"));
  CHECK(cfg.has("fullscreen"));  // false is present, and not the same as absent
}

auto test_writing() -> void {
  t::section("writing fields");
  Host h;
  Table cfg = h.lua.global<Table>("config");

  cfg.set("height", 720).set("title", std::string("renamed")).set("ratio", 1.75);

  CHECK_EQ(cfg.get<int>("height"), 720);
  CHECK_STR(cfg.get<std::string>("title"), "renamed");

  // And Lua sees the same table, not a copy.
  CHECK_OK(h.lua.script(R"LUA(
    assert(config.height == 720)
    assert(config.title == "renamed")
    assert(config.ratio == 1.75)
  )LUA"));

  cfg.erase("height");
  CHECK(!cfg.has("height"));
  CHECK_OK(h.lua.script("assert(config.height == nil)"));
}

auto test_integer_keys_and_length() -> void {
  t::section("integer keys and length");
  Host h;
  Table tags = h.lua.global<Table>("config").get<Table>("tags");

  CHECK_EQ(static_cast<int>(tags.length()), 3);
  CHECK_STR(tags.get<std::string>(1), "a");
  CHECK_STR(tags.get<std::string>(3), "c");

  tags.set(4, std::string("d"));
  CHECK_EQ(static_cast<int>(tags.length()), 4);
}

auto test_count_sees_the_hash_part() -> void {
  t::section("count covers what length ignores");
  Host h;
  Table cfg = h.lua.global<Table>("config");

  CHECK_EQ(static_cast<int>(cfg.length()), 0);  // no array part at all
  CHECK_EQ(static_cast<int>(cfg.count()), 6);
}

auto test_nested() -> void {
  t::section("nested tables");
  Host h;
  Table cfg = h.lua.global<Table>("config");

  CHECK_EQ(cfg.get<Table>("nested").get<Table>("deep").get<int>("answer"), 42);
}

auto test_create_and_hand_back() -> void {
  t::section("building a table in C++ and handing it to Lua");
  Host h;
  core::State *L = h.lua.raw();

  Table t = Table::create(L, 0, 2);
  t.set("name", std::string("from C++")).set("value", 7);

  h.lua.set_global("made", std::move(t));
  CHECK_OK(h.lua.script(R"LUA(
    assert(made.name == "from C++")
    assert(made.value == 7)
  )LUA"));
}

auto test_globals() -> void {
  t::section("the globals table");
  Host h;

  h.lua.set_global("answer", 42);
  CHECK_EQ(h.lua.global<int>("answer"), 42);
  CHECK_OK(h.lua.script("assert(answer == 42)"));

  CHECK_EQ(h.lua.global_or<int>("nothing_here", -1), -1);
  CHECK(h.lua.globals().has("config"));

  // _G really is _G, not a copy of it.
  h.lua.globals().set("via_table", std::string("yes"));
  CHECK_OK(h.lua.script("assert(via_table == 'yes')"));
}

auto test_empty_table() -> void {
  t::section("an empty Table refuses to be used");
  Table t;
  CHECK(!t.valid());
  CHECK(!static_cast<bool>(t));

  bool caught = false;
  try {
    t.get<int>("anything");
  } catch (const Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("empty Table") != std::string::npos);
  }
  CHECK(caught);
}

// A table parameter is the natural shape for a plugin's setup call.
auto test_table_as_a_parameter() -> void {
  t::section("a Table arrives as a bound function parameter");
  Host h;
  seen_width = 0;
  seen_title.clear();

  h.lua.preload({"engine", open_engine});
  CHECK_OK(h.lua.script(R"LUA(
    local engine = require("engine")
    engine.configure{ width = 640, title = "passed in" }

    local ok, err = pcall(engine.configure, 7)
    assert(not ok)
    assert(err:find("bad argument #1"), err)
  )LUA"));

  CHECK_EQ(seen_width, 640);
  CHECK_STR(seen_title, "passed in");
}

// Every operation pushes the table and must take it back off again.
auto test_stack_is_balanced() -> void {
  t::section("operations leave the stack where they found it");
  Host h;
  core::State *L = h.lua.raw();
  Table cfg = h.lua.global<Table>("config");

  const int top = core::gettop(L);
  for (int i = 0; i < 500; ++i) {
    cfg.set("counter", i);
    CHECK_EQ(cfg.get<int>("counter"), i);
    cfg.get_or<int>("missing", 0);
    cfg.has("title");
    cfg.length();
    try {
      cfg.get<int>("title");  // throws, and must still clean up
    } catch (const Error &) {
    }
  }
  CHECK_EQ(core::gettop(L), top);
}

auto test_stack_specialization() -> void {
  t::section("Stack<Table> does not borrow");
  static_assert(!luakit::Stack<Table>::borrows);

  Host h;
  core::State *L = h.lua.raw();
  core::getglobal(L, "config");
  CHECK(luakit::Stack<Table>::test(L, -1));
  core::pop(L, 1);

  core::pushinteger(L, 1);
  CHECK(!luakit::Stack<Table>::test(L, -1));
  core::pop(L, 1);
}

}  // namespace

auto main() -> int {
  test_reading_fields();
  test_missing_and_wrong_type();
  test_has();
  test_writing();
  test_integer_keys_and_length();
  test_count_sees_the_hash_part();
  test_nested();
  test_create_and_hand_back();
  test_globals();
  test_empty_table();
  test_table_as_a_parameter();
  test_stack_is_balanced();
  test_stack_specialization();
  return t::summary();
}
