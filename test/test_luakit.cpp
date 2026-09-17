// Regression tests for luakit's lifetime and error-path invariants.
//
// Correctness around the C++/Lua error-model mismatch is what luakit is for,
// so these are the tests that matter. Run under ASan/LSan/UBSan via `make
// test`: several cases assert "nothing leaked" or "no double free", which only
// the sanitizers can actually observe.

#include "check.hpp"

#include "luakit/function.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

// ---------------------------------------------------------------- fixtures

int counter_dtors = 0;

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
int thrower_dtors = 0;

struct Thrower {
  std::string name;
  std::vector<double> data;
  explicit Thrower(std::string n) : name(std::move(n)), data(100, 1.0) {
    throw std::runtime_error("constructor failed");
  }
  ~Thrower() { ++thrower_dtors; }
};

}  // namespace

template <>
struct luakit::Metatable<Counter> {
  static constexpr const char *k_name = "test.Counter";
};
template <>
struct luakit::Metatable<Thrower> {
  static constexpr const char *k_name = "test.Thrower";
};

namespace {

// ---------------------------------------------------------- bound functions

auto add(double a, double b) -> double {
  return a + b;
}
auto join(std::string a, std::string b) -> std::string {
  return a + b;
}
auto scale(std::vector<double> v, double f) -> std::vector<double> {
  for (auto &x : v) x *= f;
  return v;
}
auto lengths(std::vector<std::string> v) -> std::vector<int> {
  std::vector<int> out;
  out.reserve(v.size());
  for (const auto &s : v) out.push_back(static_cast<int>(s.size()));
  return out;
}
auto nothing(int) -> void {
}
auto two() -> std::tuple<int, std::string> {
  return {7, "seven"};
}
auto with_default(std::optional<int> n) -> int {
  return n.value_or(-1);
}
auto flip(bool b) -> bool {
  return !b;
}
auto boom() -> void {
  throw std::runtime_error("from C++");
}
auto boom_unknown() -> void {
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
    luakit::Binding<Counter>::register_class(L, counter_methods);
    luakit::Binding<Thrower>::register_class(L, thrower_methods);
    lua::aux::newlib(L, funcs);
    lua::setglobal(L, "m");
  }
  ~Fixture() { lua::close(L); }
};

// Runs a chunk, returns the error message (empty when it succeeded).
auto run(lua::State *L, const char *src) -> std::string {
  if (lua::aux::dostring(L, src) == lua::OK) return {};
  std::string msg = lua::tostring(L, -1) ? lua::tostring(L, -1) : "?";
  lua::pop(L, 1);
  return msg;
}

// Evaluates an expression and returns it as a string, for easy comparison.
auto eval(lua::State *L, const char *expr) -> std::string {
  const std::string chunk = std::string("__r = tostring(") + expr + ")";
  const std::string err = run(L, chunk.c_str());
  if (!err.empty()) return "<error> " + err;
  lua::getglobal(L, "__r");
  std::string out = lua::tostring(L, -1);
  lua::pop(L, 1);
  return out;
}

auto contains(const std::string &hay, const char *needle) -> bool {
  return hay.find(needle) != std::string::npos;
}

// -------------------------------------------------------------------- tests

auto test_scalar_roundtrip() -> void {
  t::section("scalar and string round-trips");
  Fixture f;
  CHECK_STR(eval(f.L, "m.add(2, 3)"), "5.0");
  CHECK_STR(eval(f.L, "m.join('a', 'b')"), "ab");
  CHECK_STR(eval(f.L, "m.flip(true)"), "false");
  CHECK_STR(eval(f.L, "select('#', m.nothing(1))"), "0");
}

auto test_containers() -> void {
  t::section("containers");
  Fixture f;
  CHECK_STR(eval(f.L, "table.concat(m.scale({1,2,3}, 2.5), ',')"), "2.5,5.0,7.5");
  CHECK_STR(eval(f.L, "table.concat(m.lengths({'a','bb','ccc'}), ',')"), "1,2,3");
  CHECK_STR(eval(f.L, "#m.scale({}, 2)"), "0");
}

auto test_multi_return() -> void {
  t::section("multiple return values");
  Fixture f;
  CHECK_STR(eval(f.L, "select('#', m.two())"), "2");
  CHECK_STR(eval(f.L, "select(2, m.two())"), "seven");
}

auto test_optional() -> void {
  t::section("std::optional arguments");
  Fixture f;
  CHECK_STR(eval(f.L, "m.with_default()"), "-1");     // no value
  CHECK_STR(eval(f.L, "m.with_default(nil)"), "-1");  // explicit nil
  CHECK_STR(eval(f.L, "m.with_default(5)"), "5");
  CHECK(contains(eval(f.L, "m.with_default({})"), "<error>"));
}

auto test_argument_errors() -> void {
  t::section("argument diagnostics");
  Fixture f;
  CHECK(contains(eval(f.L, "m.add('x', 1)"), "bad argument #1"));
  CHECK(contains(eval(f.L, "m.add(1)"), "bad argument #2"));
  CHECK(contains(eval(f.L, "m.flip(1)"), "bad argument #1"));
  // The container reports which element failed, not a raw stack index.
  const std::string e = eval(f.L, "m.scale({1, 'a', 3}, 2)");
  CHECK(contains(e, "bad argument #1"));
  CHECK(contains(e, "element 2"));
}

auto test_exception_boundary() -> void {
  t::section("C++ exception boundary");
  Fixture f;
  CHECK(contains(eval(f.L, "m.boom()"), "[guard]"));
  CHECK(contains(eval(f.L, "m.boom()"), "from C++"));
  CHECK(contains(eval(f.L, "m.boom_unknown()"), "unknown C++ exception"));
}

auto test_methods() -> void {
  t::section("method binding and chaining");
  Fixture f;
  CHECK_STR(eval(f.L, "m.counter():bump(3):bump(4):value()"), "7");
  CHECK_STR(eval(f.L, "m.counter():label()"), "counter");
  // reset() returns void, so the chain stops there: nothing to index.
  CHECK(contains(eval(f.L, "m.counter():bump(5):reset():value()"), "index a nil value"));
  CHECK(contains(eval(f.L, "getmetatable(m.counter()).__index.value({})"), "test.Counter expected"));
  CHECK(contains(eval(f.L, "getmetatable(m.counter()).__index.bump(m.counter())"), "bad argument #2"));
}

// The core invariant: a constructor that throws must leave no object for __gc
// to destroy, and must not double-free what the partially-built constructor
// already unwound. Without Box::live this double-frees under ASan.
auto test_throwing_constructor() -> void {
  t::section("throwing constructor leaves no half-built userdata");
  thrower_dtors = 0;
  {
    Fixture f;
    CHECK(contains(eval(f.L, "m.thrower('a-name-too-long-for-SSO-so-it-allocates')"), "constructor failed"));
    run(f.L, "collectgarbage() collectgarbage()");
  }
  CHECK_EQ(thrower_dtors, 0);  // never constructed, so never destroyed
}

auto test_destruction_exactly_once() -> void {
  t::section("each object is destroyed exactly once");
  counter_dtors = 0;
  {
    Fixture f;
    run(f.L, "for i = 1, 20 do local c = m.counter() c:bump(i) end");
    run(f.L, "collectgarbage() collectgarbage()");
    CHECK_EQ(counter_dtors, 20);
    run(f.L, "g = m.counter()");  // still reachable at close
  }
  CHECK_EQ(counter_dtors, 21);  // lua_close finalizes the survivor
}

// Argument checking runs to completion before any C++ object is built, so a
// failure part-way through cannot strand an earlier argument. LSan is what
// actually proves this.
auto test_no_leak_on_partial_arguments() -> void {
  t::section("failed argument check strands nothing");
  Fixture f;
  for (int i = 0; i < 200; ++i) {
    CHECK(contains(eval(f.L, "m.join('a-string-long-enough-to-heap-allocate', {})"), "bad argument #2"));
    CHECK(contains(eval(f.L, "m.lengths({'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', {}, 'bbbb'})"), "element 2"));
  }
}

auto test_identity() -> void {
  t::section("userdata identity");
  Fixture f;
  CHECK_STR(eval(f.L, "(function() local a = m.counter() return rawequal(a, a) end)()"), "true");
  CHECK_STR(eval(f.L,
                 "(function() local a = m.counter() local b = a b:bump(9) "
                 "return a:value() end)()"),
            "9");
}

}  // namespace

auto main() -> int {
  test_scalar_roundtrip();
  test_containers();
  test_multi_return();
  test_optional();
  test_argument_errors();
  test_exception_boundary();
  test_methods();
  test_throwing_constructor();
  test_destruction_exactly_once();
  test_no_leak_on_partial_arguments();
  test_identity();
  return t::summary();
}
