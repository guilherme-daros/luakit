// Tests for the owning Interpreter facade.

#include "check.hpp"

#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"

#include <string>
#include <utility>

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

const lua::aux::Reg widget_methods[] = {
    {"set",   luakit::method<&Widget::set>},
    {"get",   luakit::method<&Widget::get>},
    {nullptr, nullptr                     },
};
const lua::aux::Reg mod[] = {
    {"new",   luakit::ctor<Widget>},
    {"twice", luakit::fn<twice>   },
    {nullptr, nullptr             },
};

auto open_mod(lua::State *L) -> int {
  lua::aux::newlib(L, mod);
  return 1;
}

auto test_lifecycle() -> void {
  t::section("Interpreter lifecycle");
  luakit::Interpreter s;
  CHECK(s.raw() != nullptr);
  s.open_libs();
  s.script("x = 1 + 1");
  lua::getglobal(s.raw(), "x");
  CHECK_EQ(s.get<int>(-1), 2);
  lua::pop(s.raw(), 1);
}

auto test_move() -> void {
  t::section("Interpreter is movable, not copyable");
  luakit::Interpreter a;
  a.open_libs();
  a.script("y = 41");
  lua::State *raw = a.raw();

  luakit::Interpreter b(std::move(a));
  CHECK(a.raw() == nullptr);  // moved-from is emptied
  CHECK(b.raw() == raw);      // and owns the same interpreter
  b.script("y = y + 1");
  lua::getglobal(b.raw(), "y");
  CHECK_EQ(b.get<int>(-1), 42);
  lua::pop(b.raw(), 1);

  luakit::Interpreter c;
  c = std::move(b);  // must close c's original state, not leak it
  CHECK(b.raw() == nullptr);
  CHECK(c.raw() == raw);
}

auto test_errors_are_exceptions() -> void {
  t::section("script failures become luakit::Error");
  luakit::Interpreter s;
  s.open_libs();

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
  CHECK_EQ(lua::gettop(s.raw()), 0);
  s.script("z = 3");
}

auto test_preload_and_bind() -> void {
  t::section("preload and bind");
  luakit::Interpreter s;
  s.open_libs().bind<Widget>(widget_methods).preload("mod", open_mod);

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
  s.open_libs();
  const char *src = "bc = 'loaded'";
  s.script_bytecode(src, std::char_traits<char>::length(src), "@fake.lua");
  lua::getglobal(s.raw(), "bc");
  CHECK_STR(s.get<std::string>(-1), "loaded");
  lua::pop(s.raw(), 1);
}

}  // namespace

auto main() -> int {
  test_lifecycle();
  test_move();
  test_errors_are_exceptions();
  test_preload_and_bind();
  test_script_bytecode();
  return t::summary();
}
