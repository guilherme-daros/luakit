// Function: holding a Lua function in C++ and calling it back.
//
// This is the direction that makes a plugin a plugin -- the host fires a
// handler the script registered -- so most of these cases are about a callback
// outliving the call that handed it over.

#include "check.hpp"

#include "luakit/callback.hpp"
#include "luakit/interpreter.hpp"

#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace core = luakit::core;

namespace {

using luakit::Error;
using luakit::Function;
using luakit::Interpreter;

// The engine side: a plugin registers handlers, the host fires them later.
std::vector<Function> handlers;

auto on_tick(Function cb) -> void {
  handlers.push_back(std::move(cb));
}

auto apply(Function f, double x) -> double {
  return f.call<double>(x);
}

const core::aux::Reg engine[] = {
    {"on_tick", luakit::fn<on_tick>},
    {"apply",   luakit::fn<apply>  },
    {nullptr,   nullptr            },
};

auto open_engine(core::State *L) -> int {
  core::aux::newlib(L, engine);
  return 1;
}

// Pulls a global function out as a Function, the way a host would.
auto global_fn(Interpreter &lua, const char *name) -> Function {
  core::getglobal(lua.raw(), name);
  Function f = Function::pop(lua.raw());
  return f;
}

auto test_call_basics() -> void {
  t::section("calling a Lua function from C++");
  Interpreter lua;
  lua.openlibs();
  lua.script(
      "function double_it(x) return x * 2 end\n"
      "function greet(who) return 'hello, ' .. who end\n"
      "function nothing() end\n"
      "function pair() return 7, 'seven' end\n");

  CHECK_EQ(global_fn(lua, "double_it").call<int>(21), 42);
  CHECK_STR(global_fn(lua, "greet").call<std::string>("world"), "hello, world");

  global_fn(lua, "nothing").call<>();  // R defaults to void

  auto [n, s] = global_fn(lua, "pair").call<std::tuple<int, std::string>>();
  CHECK_EQ(n, 7);
  CHECK_STR(s, "seven");
}

auto test_empty_function() -> void {
  t::section("an empty Function refuses to be called");
  Function f;
  CHECK(!f.valid());
  CHECK(!static_cast<bool>(f));

  bool caught = false;
  try {
    f.call<int>();
  } catch (const Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("empty Function") != std::string::npos);
  }
  CHECK(caught);
}

auto test_error_inside_the_callback() -> void {
  t::section("an error in the callback throws, with a traceback");
  Interpreter lua;
  lua.openlibs();
  lua.script(
      "local function inner() error('plugin fault') end\n"
      "function faulty() inner() end\n");

  bool caught = false;
  try {
    global_fn(lua, "faulty").call<>();
  } catch (const Error &e) {
    caught = true;
    const std::string msg = e.what();
    CHECK(msg.find("plugin fault") != std::string::npos);
    CHECK(msg.find("stack traceback:") != std::string::npos);
  }
  CHECK(caught);
}

auto test_wrong_result_type() -> void {
  t::section("a result of the wrong type throws instead of raising");
  Interpreter lua;
  lua.openlibs();
  CHECK_OK(lua.script("function gives_table() return {} end"));

  bool caught = false;
  try {
    global_fn(lua, "gives_table").call<int>();
  } catch (const Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("expected") != std::string::npos);
  }
  CHECK(caught);
}

// The stack must come back to where it started whether the call worked or not,
// or a handler fired every frame would grow it without bound.
auto test_stack_is_balanced() -> void {
  t::section("the stack is restored on both paths");
  Interpreter lua;
  lua.openlibs();
  lua.script(
      "function ok(a, b, c) return a + b + c end\n"
      "function bad() error('nope') end\n");

  core::State *L = lua.raw();
  const int top = core::gettop(L);

  Function good = global_fn(lua, "ok");
  Function faulty = global_fn(lua, "bad");

  for (int i = 0; i < 500; ++i) {
    CHECK_EQ(good.call<int>(1, 2, 3), 6);
    try {
      faulty.call<>();
    } catch (const Error &) {
    }
  }
  CHECK_EQ(core::gettop(L), top);
}

// The whole point: the closure was created inside require() and the call that
// registered it has long since returned.
auto test_handler_outlives_registration() -> void {
  t::section("a registered handler outlives the call that passed it");
  handlers.clear();

  Interpreter lua;
  lua.openlibs().preload({"engine", open_engine});
  CHECK_OK(lua.script(R"LUA(
    local engine = require("engine")
    total = 0
    engine.on_tick(function(dt) total = total + dt end)
    engine.on_tick(function(dt) total = total + dt * 10 end)
  )LUA"));

  CHECK_EQ(static_cast<int>(handlers.size()), 2);

  // Nothing in Lua refers to those closures any more.
  for (int i = 0; i < 20; ++i) lua.script("collectgarbage('collect')");

  for (int frame = 0; frame < 3; ++frame) {
    for (const auto &h : handlers) h.call<void>(2);
  }

  core::getglobal(lua.raw(), "total");
  CHECK_EQ(luakit::get<int>(lua.raw(), -1), 66);  // 3 * (2 + 20)
  core::pop(lua.raw(), 1);

  handlers.clear();
}

// A Function passed straight through as a parameter and called within the same
// bound call, rather than stored.
auto test_callback_as_parameter() -> void {
  t::section("a callback used within the call that received it");
  Interpreter lua;
  lua.openlibs().preload({"engine", open_engine});
  CHECK_OK(lua.script(R"LUA(
    local engine = require("engine")
    assert(engine.apply(function(x) return x + 1 end, 41) == 42)

    -- A non-function argument is rejected by name.
    local ok, err = pcall(engine.apply, 7, 1)
    assert(not ok)
    assert(err:find("bad argument #1"), err)
  )LUA"));
}

// An error thrown out of a callback fired from inside a bound C++ function has
// to become a Lua error, not unwind through Lua's C frames. guard<> is what
// turns it back, and this checks the seam holds.
auto test_error_propagates_back_through_cpp() -> void {
  t::section("a callback fault crossing a C++ frame becomes a Lua error");
  Interpreter lua;
  lua.openlibs().preload({"engine", open_engine});
  CHECK_OK(lua.script(R"LUA(
    local engine = require("engine")
    local ok, err = pcall(engine.apply, function() error("from the callback") end, 1)
    assert(not ok)
    assert(err:find("from the callback"), err)
  )LUA"));
}

auto test_move_semantics() -> void {
  t::section("Function is movable and the source is emptied");
  Interpreter lua;
  lua.openlibs();
  CHECK_OK(lua.script("function five() return 5 end"));

  Function a = global_fn(lua, "five");
  Function b(std::move(a));
  CHECK(!a.valid());
  CHECK_EQ(b.call<int>(), 5);

  Function c = global_fn(lua, "five");
  c = std::move(b);
  CHECK(!b.valid());
  CHECK_EQ(c.call<int>(), 5);

  c.reset();
  CHECK(!c.valid());
}

auto test_stack_specialization() -> void {
  t::section("Stack<Function> is strict and does not borrow");
  static_assert(!luakit::Stack<Function>::borrows);

  Interpreter lua;
  lua.openlibs();
  core::State *L = lua.raw();

  CHECK_OK(lua.script("function f() end"));
  core::getglobal(L, "f");
  CHECK(luakit::Stack<Function>::test(L, -1));
  core::pop(L, 1);

  // A callable table is not a function.
  CHECK_OK(lua.script("t = setmetatable({}, {__call = function() end})"));
  core::getglobal(L, "t");
  CHECK(!luakit::Stack<Function>::test(L, -1));
  core::pop(L, 1);
}

}  // namespace

auto main() -> int {
  test_call_basics();
  test_empty_function();
  test_error_inside_the_callback();
  test_wrong_result_type();
  test_stack_is_balanced();
  test_handler_outlives_registration();
  test_callback_as_parameter();
  test_error_propagates_back_through_cpp();
  test_move_semantics();
  test_stack_specialization();
  return t::summary();
}
