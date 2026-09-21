// Library: building a module table from free functions with .fn<>(),
// .overload<>() and .raw_fn(), no class involved.

#include "check.hpp"

#include "luakit/interpreter.hpp"
#include "luakit/library.hpp"

#include <string>

namespace core = luakit::core;

namespace {

auto sum(double a, double b) -> double {
  return a + b;
}

auto greet(std::string name) -> std::string {
  return "hi " + name;
}

auto length(double x) -> double {
  return x < 0 ? -x : x;
}
auto length(double x, double y) -> double {
  return (x * x + y * y);
}

auto open_mod(core::State *L) -> int {
  return luakit::Library(L)
      .fn<sum>("sum")
      .fn<greet>("greet")
      .overload<static_cast<double (*)(double)>(length), static_cast<double (*)(double, double)>(length)>("length")
      .raw_fn("sum2", luakit::fn<sum>)
      .build_module();
}

auto test_fn_and_raw_fn() -> void {
  t::section("a module built from free functions");
  luakit::Interpreter s;
  s.open_libs().preload({"mod", open_mod});

  CHECK_OK(s.script(R"LUA(
    local m = require("mod")
    assert(m.sum(2, 3) == 5)
    assert(m.greet("world") == "hi world")
    assert(m.length(-4) == 4)
    assert(m.length(3, 4) == 25)
    assert(m.sum2(4, 5) == 9)
  )LUA"));
}

}  // namespace

auto main() -> int {
  test_fn_and_raw_fn();
  return t::summary();
}
