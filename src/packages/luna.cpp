#include "luna.hpp"

#include "luakit/library.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace core = luakit::core;

namespace luna {

// Plain C++. Nothing here knows about Lua: argument checking, conversion and
// return handling all come from the signature via luakit::fn.

auto sum(double a, double b) -> double {
  return a + b;
}

auto greet(std::string_view name) -> std::string {
  return "hello, " + std::string(name) + ", from C++";
}

auto scale(std::vector<double> v, double factor) -> std::vector<double> {
  for (auto &x : v) x *= factor;
  return v;
}

auto boom() -> void {
  throw std::runtime_error("a std::runtime_error from C++");
}

}  // namespace luna

auto luaopen_luna(core::State *L) -> int {
  using namespace luna;

  return luakit::Library(L, "luna")
      .fn<sum>("sum")
      .fn<greet>("greet")
      .fn<scale>("scale")
      .fn<boom>("boom")
      .build_module();
}
