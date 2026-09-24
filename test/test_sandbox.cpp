// Resource limits, restricted environments, and handles that outlive the
// interpreter they came from.

#include "check.hpp"

#include "luakit/callback.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/table.hpp"

#include <string>
#include <vector>

namespace {

auto failure(luakit::Interpreter &lua, const char *code) -> std::string {
  try {
    lua.script(code);
  } catch (const luakit::Error &e) {
    return e.what();
  }
  return {};
}

auto contains(const std::string &hay, const char *needle) -> bool {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

auto main() -> int {
  t::section("an instruction budget stops a script that will not return");
  {
    luakit::Interpreter lua(luakit::Limits{.memory = 0, .instructions = 200000});
    lua.openlibs();
    CHECK(contains(failure(lua, "while true do end"), "instruction budget"));

    // Re-armed per call: one runaway must not disable the plugin.
    CHECK_OK(lua.script("local t = {} for i = 1, 100 do t[i] = i end"));
  }

  t::section("the budget reaches a coroutine the script made itself");
  {
    luakit::Interpreter lua(luakit::Limits{.memory = 0, .instructions = 200000});
    lua.openlibs();
    const std::string err = failure(lua, "local c = coroutine.wrap(function() while true do end end) c()");
    CHECK(contains(err, "instruction budget"));
  }

  t::section("no budget means no hook and no limit");
  {
    luakit::Interpreter lua;
    lua.openlibs();
    CHECK_OK(lua.script("local n = 0 for i = 1, 500000 do n = n + i end"));
  }

  t::section("a memory cap surfaces as an error, not a crash");
  {
    luakit::Interpreter lua(luakit::Limits{.memory = 4u << 20});
    lua.openlibs();
    const std::string err = failure(lua, "local t = {} while true do t[#t + 1] = string.rep('x', 4096) end");
    CHECK(contains(err, "memory"));

    // Still usable afterwards: the state unwound rather than died.
    CHECK_EQ(lua.eval<int>("return 1 + 1"), 2);
    CHECK(lua.memory_used() > 0);
    CHECK(lua.memory_used() <= (4u << 20));
  }

  t::section("memory_used tracks real allocation");
  {
    luakit::Interpreter lua;
    lua.openlibs();
    const std::size_t before = lua.memory_used();
    lua.script("big = string.rep('x', 2000000)");
    CHECK(lua.memory_used() > before + 1000000);
    lua.script("big = nil");
    lua.gc().collect();
    CHECK(lua.memory_used() < before + 1000000);
  }

  t::section("make_env withholds everything not asked for");
  {
    luakit::Interpreter lua;
    lua.openlibs();
    auto env = lua.make_env({"assert", "tostring", "string"});

    CHECK_OK(lua.script_in(env, "assert(tostring(1) == '1')"));
    CHECK_OK(lua.script_in(env, "assert(os == nil, 'os leaked')"));
    CHECK_OK(lua.script_in(env, "assert(require == nil, 'require leaked')"));
    CHECK_OK(lua.script_in(env, "assert(string.rep('a', 2) == 'aa')"));

    // _G points at the sandbox, not at the real globals.
    CHECK_OK(lua.script_in(env, "assert(_G.os == nil, '_G leaked the real globals')"));
    CHECK_OK(lua.script_in(env, "assert(_G.tostring ~= nil)"));
  }

  t::section("a sandboxed chunk cannot reach the real globals by writing");
  {
    luakit::Interpreter lua;
    lua.openlibs();
    auto env = lua.make_env({});
    lua.script_in(env, "planted = 'from the sandbox'");
    CHECK_EQ(lua.eval<bool>("return planted == nil"), true);
  }

  t::section("handles outlive the interpreter that made them");
  {
    luakit::Function cb;
    luakit::Table cfg;
    {
      luakit::Interpreter lua;
      lua.openlibs();
      cb = lua.eval<luakit::Function>("return function(x) return x * 2 end");
      cfg = lua.eval<luakit::Table>("return { width = 1280 }");

      CHECK_EQ(cb.call<int>(21), 42);
      CHECK_EQ(cfg.get<int>("width"), 1280);
    }

    // The state is gone. Both objects say so rather than touching it, and
    // their destructors -- running at the end of this scope, against a closed
    // interpreter -- are the case this whole mechanism exists for.
    bool said_so = false;
    try {
      cb.call<int>(1);
    } catch (const luakit::Error &e) {
      said_so = contains(e.what(), "closed");
    }
    CHECK(said_so);

    said_so = false;
    try {
      (void)cfg.get<int>("width");
    } catch (const luakit::Error &e) {
      said_so = contains(e.what(), "closed");
    }
    CHECK(said_so);
  }

  t::section("close() is idempotent and early-closing works");
  {
    luakit::Interpreter lua;
    lua.openlibs();
    lua.script("x = 1");
    lua.close();
    lua.close();
    CHECK(true);  // reaching here without a crash is the check
  }

  t::section("gc control");
  {
    luakit::Interpreter lua;
    lua.openlibs();
    CHECK(lua.gc().running());
    lua.gc().stop();
    CHECK(!lua.gc().running());
    lua.gc().restart();
    CHECK(lua.gc().running());

    lua.script("junk = {} for i = 1, 10000 do junk[i] = {i} end junk = nil");
    lua.gc().collect();
    CHECK(lua.gc().bytes() > 0);

    lua.gc().generational(20, 100);
    CHECK_OK(lua.script("local t = {} for i = 1, 1000 do t[i] = i end"));
    lua.gc().incremental(200, 100, 13);
    CHECK_OK(lua.gc().step(1));
  }

  return t::summary();
}
