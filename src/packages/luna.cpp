#include "luna.hpp"

#include "luakit/function.hpp"

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

const core::aux::Reg funcs[] = {
    {"sum",   luakit::fn<sum>  },
    {"greet", luakit::fn<greet>},
    {"scale", luakit::fn<scale>},
    {"boom",  luakit::fn<boom> },
    {nullptr, nullptr          },
};

}  // namespace luna

extern "C" auto luaopen_luna(core::State *L) -> int {
  core::aux::newlib(L, luna::funcs);
  return 1;
}
