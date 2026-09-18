// Coroutine: driving a Lua coroutine from C++.
//
// The anchoring case is the one that justifies the class. A thread from
// lua_newthread is garbage collected, so without the registry reference the
// collector frees it and resuming becomes a use-after-free -- which is why
// this runs under ASan.

#include "check.hpp"

#include "luakit/coroutine.hpp"
#include "luakit/function.hpp"

#include <string>
#include <utility>

namespace core = luakit::core;

namespace {

using luakit::Coroutine;
using luakit::Error;
using luakit::Interpreter;

// The scheduler side of a yielding function: sleep() records what it was asked
// for and suspends; the host resumes it when the time has passed.
double slept_for = 0;
int sleep_calls = 0;

auto sleep_for(double seconds) -> double {
  slept_for += seconds;
  ++sleep_calls;
  return seconds;  // yielded to whoever is driving, so it knows how long to wait
}

auto pause_here() -> void {
}

const core::aux::Reg scheduler[] = {
    {"sleep", luakit::yielding<sleep_for> },
    {"pause", luakit::yielding<pause_here>},
    {nullptr, nullptr                     },
};

auto open_scheduler(core::State *L) -> int {
  core::aux::newlib(L, scheduler);
  return 1;
}

// A fresh interpreter with a few coroutine bodies defined as globals.
struct Host {
  Interpreter lua;
  Host() {
    lua.open_libs();
    lua.script(R"LUA(
      function counter()
        for i = 1, 3 do coroutine.yield(i) end
      end

      function greeter(name)
        coroutine.yield("hello, " .. name)
        coroutine.yield("bye, " .. name)
      end

      -- Echoes back whatever the resumer passes into yield().
      function echo()
        local got = coroutine.yield("ready")
        coroutine.yield("got " .. tostring(got))
      end

      function exploder()
        coroutine.yield(1)
        error("boom from Lua")
      end

      function returns_only()
        return 42
      end

      function yields_a_table()
        coroutine.yield({})
      end

      not_a_function = 7
    )LUA");
  }
};

auto test_generator() -> void {
  t::section("generator yields then finishes");
  Host h;
  Coroutine co(h.lua, "counter");

  CHECK(!co.done());
  CHECK_EQ(co.resume<int>().value_or(-1), 1);
  CHECK_EQ(co.resume<int>().value_or(-1), 2);
  CHECK_EQ(co.resume<int>().value_or(-1), 3);
  CHECK(!co.resume<int>().has_value());  // ran to completion
  CHECK(co.done());
  CHECK(co.status() == luakit::Status::finished);
}

auto test_void_resume() -> void {
  t::section("R = void reports suspended as a bool");
  Host h;
  Coroutine co(h.lua, "counter");
  int steps = 0;
  while (co.resume()) ++steps;
  CHECK_EQ(steps, 3);
  CHECK(co.done());
}

auto test_arguments_in() -> void {
  t::section("arguments reach the function on first resume");
  Host h;
  Coroutine co(h.lua, "greeter");
  CHECK_STR(co.resume<std::string>("world").value_or(""), "hello, world");
  CHECK_STR(co.resume<std::string>().value_or(""), "bye, world");
  CHECK(!co.resume<std::string>().has_value());
}

auto test_values_back_into_yield() -> void {
  t::section("later resumes feed the result of coroutine.yield");
  Host h;
  Coroutine co(h.lua, "echo");
  CHECK_STR(co.resume<std::string>().value_or(""), "ready");
  CHECK_STR(co.resume<std::string>(99).value_or(""), "got 99");
}

auto test_error_throws() -> void {
  t::section("an error inside the coroutine throws Error");
  Host h;
  Coroutine co(h.lua, "exploder");
  CHECK_EQ(co.resume<int>().value_or(-1), 1);

  bool caught = false;
  try {
    co.resume<int>();
  } catch (const Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("boom from Lua") != std::string::npos);
  }
  CHECK(caught);
  CHECK(co.done());  // a failed coroutine is not resumable
}

auto test_resume_after_finish() -> void {
  t::section("resuming a finished coroutine is safe");
  Host h;
  Coroutine co(h.lua, "returns_only");
  CHECK(!co.resume<int>().has_value());  // returns without ever yielding
  CHECK(co.done());
  for (int i = 0; i < 5; ++i) CHECK(!co.resume<int>().has_value());
}

auto test_type_mismatch_throws() -> void {
  t::section("a wrongly typed yield throws instead of raising");
  Host h;
  Coroutine co(h.lua, "yields_a_table");
  bool caught = false;
  try {
    co.resume<int>();
  } catch (const Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("expected") != std::string::npos);
  }
  CHECK(caught);
}

auto test_bad_construction() -> void {
  t::section("constructing from a non-function throws");
  Host h;
  bool caught = false;
  try {
    Coroutine co(h.lua, "not_a_function");
  } catch (const Error &) {
    caught = true;
  }
  CHECK(caught);

  caught = false;
  try {
    Coroutine co(h.lua, "no_such_global");
  } catch (const Error &) {
    caught = true;
  }
  CHECK(caught);
}

// Without aux::ref in the constructor the thread is unreachable from Lua, so
// these collections free it and the resume below reads freed memory.
auto test_survives_collection() -> void {
  t::section("the thread survives garbage collection");
  Host h;
  Coroutine co(h.lua, "counter");

  CHECK_EQ(co.resume<int>().value_or(-1), 1);
  for (int i = 0; i < 20; ++i) h.lua.script("collectgarbage('collect')");
  CHECK_EQ(co.resume<int>().value_or(-1), 2);
  h.lua.script("collectgarbage('collect') collectgarbage('collect')");
  CHECK_EQ(co.resume<int>().value_or(-1), 3);
  CHECK(!co.resume<int>().has_value());
}

// Every Coroutine takes a registry slot; the destructor must give it back.
auto test_no_registry_leak() -> void {
  t::section("destruction releases the registry anchor");
  Host h;

  auto registry_size = [&] {
    h.lua.script("__n = 0 for _ in pairs(debug.getregistry()) do __n = __n + 1 end");
    core::getglobal(h.lua.raw(), "__n");
    const int n = luakit::get<int>(h.lua.raw(), -1);
    core::pop(h.lua.raw(), 1);
    return n;
  };

  // luaL_unref recycles slots through a free list threaded into the registry
  // itself, so a released slot keeps its key and only its value changes. The
  // very first ref is what allocates both the list head and that slot, which
  // is why the baseline is taken after one cycle rather than before it.
  {
    Coroutine warmup(h.lua, "counter");
    warmup.resume<int>();
  }

  const int before = registry_size();
  for (int i = 0; i < 50; ++i) {
    Coroutine co(h.lua, "counter");
    co.resume<int>();
  }

  // Steady state: 50 more anchors must all come out of that one free slot.
  CHECK_EQ(registry_size(), before);
}

auto test_move_semantics() -> void {
  t::section("move leaves the source empty and the target usable");
  Host h;
  Coroutine a(h.lua, "counter");
  CHECK_EQ(a.resume<int>().value_or(-1), 1);

  Coroutine b(std::move(a));
  CHECK(a.done());  // moved-from is inert
  CHECK(a.raw() == nullptr);
  CHECK_EQ(b.resume<int>().value_or(-1), 2);

  Coroutine c(h.lua, "counter");
  c = std::move(b);  // must release c's own anchor, not leak it
  CHECK(b.raw() == nullptr);
  CHECK_EQ(c.resume<int>().value_or(-1), 3);
  CHECK(!c.resume<int>().has_value());
}

auto test_from_stack() -> void {
  t::section("from_stack copies a function off an existing stack");
  Host h;
  core::State *L = h.lua.raw();
  core::getglobal(L, "counter");
  const int top_before = core::gettop(L);

  Coroutine co = Coroutine::from_stack(L, -1);
  CHECK_EQ(core::gettop(L), top_before);  // the caller's stack is untouched
  CHECK_EQ(co.resume<int>().value_or(-1), 1);

  core::pop(L, 1);
}

// A bound C++ function that suspends its caller. Without lua_yieldk a script
// could only wait by yielding from Lua itself, which means the host cannot own
// the scheduling.
auto test_yielding_function() -> void {
  t::section("a bound C++ function can yield");
  Interpreter lua;
  lua.open_libs().preload({"sched", open_scheduler});
  slept_for = 0;
  sleep_calls = 0;

  lua.script(R"LUA(
    local sched = require("sched")
    log = {}
    function task()
      log[#log + 1] = "start"
      local waited = sched.sleep(2)
      log[#log + 1] = "woke after " .. tostring(waited)
      sched.sleep(3)
      log[#log + 1] = "done"
    end
  )LUA");

  Coroutine co(lua, "task");

  // Each resume runs until the next sleep, which yields the requested delay.
  CHECK_EQ(co.resume<double>().value_or(-1), 2.0);
  CHECK_EQ(sleep_calls, 1);

  // Whatever the host passes back becomes the result of the sleep() call.
  CHECK_EQ(co.resume<double>(2.0).value_or(-1), 3.0);
  CHECK_EQ(sleep_calls, 2);

  CHECK(!co.resume<double>(3.0).has_value());  // ran to completion
  CHECK(co.done());
  CHECK_EQ(slept_for, 5.0);

  lua.script(R"LUA(
    assert(#log == 3, #log)
    assert(log[1] == "start", log[1])
    assert(log[2] == "woke after 2.0", log[2])
    assert(log[3] == "done", log[3])
  )LUA");
}

// One that yields nothing still suspends, and still resumes cleanly.
auto test_yielding_without_a_value() -> void {
  t::section("a yielding function with no return value");
  Interpreter lua;
  lua.open_libs().preload({"sched", open_scheduler});

  lua.script(R"LUA(
    local sched = require("sched")
    function twostep()
      step = 1
      sched.pause()
      step = 2
    end
  )LUA");

  Coroutine co(lua, "twostep");
  CHECK(co.resume());  // suspended
  CHECK_EQ(lua.global<int>("step"), 1);
  CHECK(!co.resume());  // finished
  CHECK_EQ(lua.global<int>("step"), 2);
}

auto test_yielding_outside_a_coroutine() -> void {
  t::section("yielding outside a coroutine is refused");
  Interpreter lua;
  lua.open_libs().preload({"sched", open_scheduler});

  lua.script(R"LUA(
    local sched = require("sched")
    local ok, err = pcall(sched.sleep, 1)
    assert(not ok)
    assert(err:find("inside a coroutine"), err)
  )LUA");
}

}  // namespace

auto main() -> int {
  test_generator();
  test_void_resume();
  test_arguments_in();
  test_values_back_into_yield();
  test_error_throws();
  test_resume_after_finish();
  test_type_mismatch_throws();
  test_bad_construction();
  test_survives_collection();
  test_no_registry_leak();
  test_move_semantics();
  test_from_stack();
  test_yielding_function();
  test_yielding_without_a_value();
  test_yielding_outside_a_coroutine();
  return t::summary();
}
