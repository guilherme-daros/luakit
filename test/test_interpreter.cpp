// Tests for the owning Interpreter facade.

#include "check.hpp"

#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"

#include <string>
#include <utility>

namespace core = luakit::core;

namespace {

auto twice(int n) -> int {
  return n * 2;
}

struct Widget {
  int v = 0;
  auto set(int n) -> Widget & {
    v = n;
    return *this;
  }
  auto get() const -> int { return v; }
};

}  // namespace

template <>
struct luakit::Metatable<Widget> {
  static constexpr const char *k_name = "test.Widget";
};

namespace {

const core::aux::Reg widget_methods[] = {
    {"set",   luakit::method<&Widget::set>},
    {"get",   luakit::method<&Widget::get>},
    {nullptr, nullptr                     },
};
const core::aux::Reg mod[] = {
    {"new",   luakit::ctor<Widget>},
    {"twice", luakit::fn<twice>   },
    {nullptr, nullptr             },
};

auto open_mod(core::State *L) -> int {
  core::aux::newlib(L, mod);
  return 1;
}

auto test_lifecycle() -> void {
  t::section("Interpreter lifecycle");
  luakit::Interpreter s;
  CHECK(s.raw() != nullptr);
  s.openlibs();
  s.script("x = 1 + 1");
  CHECK_EQ(s.global<int>("x"), 2);
}

auto test_move() -> void {
  t::section("Interpreter is movable, not copyable");
  luakit::Interpreter a;
  a.openlibs();
  a.script("y = 41");
  core::State *raw = a.raw();

  luakit::Interpreter b(std::move(a));
  CHECK(a.raw() == nullptr);  // moved-from is emptied
  CHECK(b.raw() == raw);      // and owns the same interpreter
  b.script("y = y + 1");
  CHECK_EQ(b.global<int>("y"), 42);

  luakit::Interpreter c;
  c = std::move(b);  // must close c's original state, not leak it
  CHECK(b.raw() == nullptr);
  CHECK(c.raw() == raw);
}

auto test_errors_are_exceptions() -> void {
  t::section("script failures become luakit::Error");
  luakit::Interpreter s;
  s.openlibs();

  bool caught = false;
  try {
    s.script("this is not lua");
  } catch (const luakit::Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("cannot load") != std::string::npos);
  }
  CHECK(caught);

  caught = false;
  try {
    s.script("error('boom')");
  } catch (const luakit::Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("boom") != std::string::npos);
  }
  CHECK(caught);

  // The state stays usable, and the failed chunk left nothing behind.
  CHECK_EQ(core::gettop(s.raw()), 0);
  s.script("z = 3");
}

// The frames are gone by the time pcall returns, so this only works if the
// message handler ran at the point of the error.
auto test_runtime_error_has_a_traceback() -> void {
  t::section("a runtime error carries a traceback");
  luakit::Interpreter s;
  s.openlibs();

  bool caught = false;
  try {
    s.script(
        "local function inner() error('deep') end\n"
        "local function outer() inner() end\n"
        "outer()\n");
  } catch (const luakit::Error &e) {
    caught = true;
    const std::string msg = e.what();
    CHECK(msg.find("deep") != std::string::npos);
    CHECK(msg.find("stack traceback:") != std::string::npos);
    // Three frames deep: the reason the handler is worth having at all.
    CHECK(msg.find("inner") != std::string::npos);
    CHECK(msg.find("outer") != std::string::npos);
  }
  CHECK(caught);
}

// A table thrown by error{} has no string form, and must not be swallowed.
auto test_non_string_error_object() -> void {
  t::section("a non-string error object is still reported");
  luakit::Interpreter s;
  s.openlibs();

  bool caught = false;
  try {
    s.script("error(setmetatable({}, {}))");
  } catch (const luakit::Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("error object is a table value") != std::string::npos);
  }
  CHECK(caught);

  // One that renders itself speaks for itself instead.
  caught = false;
  try {
    s.script("error(setmetatable({}, {__tostring = function() return 'custom form' end}))");
  } catch (const luakit::Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("custom form") != std::string::npos);
  }
  CHECK(caught);
}

auto test_preload_and_bind() -> void {
  t::section("preload and bind");
  luakit::Interpreter s;
  s.openlibs().bind<Widget>(widget_methods).preload({"mod", open_mod});

  // Not loaded until required.
  s.script("assert(package.loaded.mod == nil)");
  s.script("assert(rawget(_G, 'mod') == nil)");

  s.script(R"LUA(
    local m = require("mod")
    assert(m.twice(21) == 42)
    local w = m.new()
    assert(w:set(5):get() == 5)
    assert(require("mod") == m)
  )LUA");
}

auto test_script_bytecode() -> void {
  t::section("script_bytecode accepts plain source too");
  luakit::Interpreter s;
  s.openlibs();
  const char *src = "bc = 'loaded'";
  s.script_bytecode(src, std::char_traits<char>::length(src), "@fake.lua");
  CHECK_STR(s.global<std::string>("bc"), "loaded");
}

// Hot reload: editing a plugin without restarting the host. The opener stands
// in for a changing source file, since what matters is that the module is
// really rebuilt rather than handed back from the cache.
int open_count = 0;

auto open_versioned(core::State *L) -> int {
  ++open_count;
  core::newtable(L);
  core::pushinteger(L, open_count);
  core::setfield(L, -2, "version");
  return 1;
}

auto test_unload_and_reload() -> void {
  t::section("a module can be unloaded and reloaded");
  luakit::Interpreter s;
  open_count = 0;
  s.openlibs().preload({"plugin", open_versioned});

  CHECK(!s.loaded("plugin"));

  s.script("p = require('plugin')");
  CHECK(s.loaded("plugin"));
  CHECK_EQ(open_count, 1);

  // require is cached, so a second one must not run the opener again.
  s.script("assert(require('plugin') == p)");
  CHECK_EQ(open_count, 1);

  s.unload("plugin");
  CHECK(!s.loaded("plugin"));
  CHECK_EQ(open_count, 1);  // unloading alone does not re-run it

  s.script("local fresh = require('plugin') assert(fresh.version == 2) assert(fresh ~= p)");
  CHECK_EQ(open_count, 2);

  // reload does both halves in one go.
  s.reload("plugin");
  CHECK_EQ(open_count, 3);
  CHECK(s.loaded("plugin"));
  s.script("assert(require('plugin').version == 3)");

  // Unloading something that was never loaded is not an error.
  s.unload("never_seen");
}

auto test_reload_failure_leaves_it_unloaded() -> void {
  t::section("a module that fails to reload is left unloaded");
  luakit::Interpreter s;
  s.openlibs();
  s.script("package.preload.broken = function() error('bad plugin') end");

  bool caught = false;
  try {
    s.reload("broken");
  } catch (const luakit::Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("bad plugin") != std::string::npos);
  }
  CHECK(caught);
  CHECK(!s.loaded("broken"));

  // The interpreter is still usable, and the failed require left nothing.
  CHECK_EQ(core::gettop(s.raw()), 0);
  s.script("after = 1");
  CHECK_EQ(s.global<int>("after"), 1);
}

auto test_reload_needs_the_libraries() -> void {
  t::section("reload says so when require is missing");
  luakit::Interpreter s;  // deliberately no openlibs

  bool caught = false;
  try {
    s.reload("anything");
  } catch (const luakit::Error &e) {
    caught = true;
    CHECK(std::string(e.what()).find("openlibs") != std::string::npos);
  }
  CHECK(caught);
}

auto test_openlib_opens_only_what_was_asked_for() -> void {
  t::section("openlib opens one library, not all of them");
  luakit::Interpreter s;
  s.openlib(luakit::Lib::base);  // for assert() itself, used below
  s.openlib(luakit::Lib::string);

  // string itself is there, both as a global and registered as loaded --
  // the same two places openlibs would have put it.
  s.script("assert(string.upper('ok') == 'OK')");
  CHECK(s.loaded("string"));

  // table was never opened, so it is absent from both -- openlib is not
  // secretly opening everything and hiding the rest.
  s.script("assert(table == nil)");
  CHECK(!s.loaded("table"));
}

}  // namespace

auto main() -> int {
  test_lifecycle();
  test_move();
  test_errors_are_exceptions();
  test_runtime_error_has_a_traceback();
  test_non_string_error_object();
  test_preload_and_bind();
  test_script_bytecode();
  test_unload_and_reload();
  test_reload_failure_leaves_it_unloaded();
  test_reload_needs_the_libraries();
  test_openlib_opens_only_what_was_asked_for();
  return t::summary();
}
