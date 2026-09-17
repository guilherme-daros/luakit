// fn / method / ctor: deducing Lua bindings from C++ signatures, and the
// guard<> exception boundary they route through.

#include "fixture.hpp"

using tf::contains;
using tf::eval;
using tf::Fixture;

namespace {

auto test_multi_return() -> void {
  t::section("multiple return values");
  Fixture f;
  CHECK_STR(eval(f.L, "select('#', m.two())"), "2");
  CHECK_STR(eval(f.L, "select(2, m.two())"), "seven");
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

}  // namespace

auto main() -> int {
  test_multi_return();
  test_exception_boundary();
  test_methods();
  return t::summary();
}
