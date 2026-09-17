// Shared harness for the luakit test suites.
//
// Each suite is its own binary, so everything here is inline and gets exactly
// one definition per executable.

#pragma once

#include "check.hpp"

#include "luakit/function.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace tf {

inline int counter_dtors = 0;

struct Counter {
  long n = 0;
  ~Counter() { ++counter_dtors; }
  auto bump(long by) -> Counter & {
    n += by;
    return *this;
  }
  auto value() const -> long { return n; }
  auto label() const -> std::string { return "counter"; }
  auto reset() -> void { n = 0; }
};

// Allocates, then fails. Members are constructed and unwound before the body
// throws, which is exactly the half-built case Box's live flag exists for.
inline int thrower_dtors = 0;

struct Thrower {
  std::string name;
  std::vector<double> data;
  explicit Thrower(std::string n) : name(std::move(n)), data(100, 1.0) {
    throw std::runtime_error("constructor failed");
  }
  ~Thrower() { ++thrower_dtors; }
};

}  // namespace tf

template <>
struct luakit::Metatable<tf::Counter> {
  static constexpr const char *k_name = "test.Counter";
};
template <>
struct luakit::Metatable<tf::Thrower> {
  static constexpr const char *k_name = "test.Thrower";
};

namespace tf {

// ---------------------------------------------------------- bound functions

inline auto add(double a, double b) -> double {
  return a + b;
}
inline auto join(std::string a, std::string b) -> std::string {
  return a + b;
}
inline auto scale(std::vector<double> v, double f) -> std::vector<double> {
  for (auto &x : v) x *= f;
  return v;
}
inline auto lengths(std::vector<std::string> v) -> std::vector<int> {
  std::vector<int> out;
  out.reserve(v.size());
  for (const auto &s : v) out.push_back(static_cast<int>(s.size()));
  return out;
}
inline auto nothing(int) -> void {
}
inline auto two() -> std::tuple<int, std::string> {
  return {7, "seven"};
}
inline auto with_default(std::optional<int> n) -> int {
  return n.value_or(-1);
}
inline auto flip(bool b) -> bool {
  return !b;
}
inline auto boom() -> void {
  throw std::runtime_error("from C++");
}
inline auto boom_unknown() -> void {
  throw 42;
}

const lua::aux::Reg funcs[] = {
    {"add", luakit::fn<add>},
    {"join", luakit::fn<join>},
    {"scale", luakit::fn<scale>},
    {"lengths", luakit::fn<lengths>},
    {"nothing", luakit::fn<nothing>},
    {"two", luakit::fn<two>},
    {"with_default", luakit::fn<with_default>},
    {"flip", luakit::fn<flip>},
    {"boom", luakit::fn<boom>},
    {"boom_unknown", luakit::fn<boom_unknown>},
    {"counter", luakit::ctor<Counter>},
    {"thrower", luakit::ctor<Thrower, std::string>},
    {nullptr, nullptr},
};

const lua::aux::Reg counter_methods[] = {
    {"bump",  luakit::method<&Counter::bump> },
    {"value", luakit::method<&Counter::value>},
    {"label", luakit::method<&Counter::label>},
    {"reset", luakit::method<&Counter::reset>},
    {nullptr, nullptr                        },
};

const lua::aux::Reg thrower_methods[] = {
    {nullptr, nullptr}
};

// ------------------------------------------------------------------ helpers

struct Fixture {
  lua::State *L;
  Fixture() : L(lua::aux::newstate()) {
    lua::aux::openlibs(L);
    luakit::Userdata<Counter>::register_class(L, counter_methods);
    luakit::Userdata<Thrower>::register_class(L, thrower_methods);
    lua::aux::newlib(L, funcs);
    lua::setglobal(L, "m");
  }
  ~Fixture() { lua::close(L); }
};

// Runs a chunk, returns the error message (empty when it succeeded).
inline auto run(lua::State *L, const char *src) -> std::string {
  if (lua::aux::dostring(L, src) == lua::OK) return {};
  std::string msg = lua::tostring(L, -1) ? lua::tostring(L, -1) : "?";
  lua::pop(L, 1);
  return msg;
}

// Evaluates an expression and returns it as a string, for easy comparison.
inline auto eval(lua::State *L, const char *expr) -> std::string {
  const std::string chunk = std::string("__r = tostring(") + expr + ")";
  const std::string err = run(L, chunk.c_str());
  if (!err.empty()) return "<error> " + err;
  lua::getglobal(L, "__r");
  std::string out = lua::tostring(L, -1);
  lua::pop(L, 1);
  return out;
}

inline auto contains(const std::string &hay, const char *needle) -> bool {
  return hay.find(needle) != std::string::npos;
}

}  // namespace tf
