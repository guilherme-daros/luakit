// Stack<T>: converting C++ values to and from the Lua stack.
//
// Run under ASan/LSan/UBSan: the last case asserts "nothing leaked", which
// only a sanitizer can observe.

#include "fixture.hpp"

using tf::contains;
using tf::eval;
using tf::Fixture;

namespace {

auto test_scalar_roundtrip() -> void {
  t::section("scalar and string round-trips");
  Fixture f;
  CHECK_STR(eval(f.L, "m.add(2, 3)"), "5.0");
  CHECK_STR(eval(f.L, "m.join('a', 'b')"), "ab");
  CHECK_STR(eval(f.L, "m.flip(true)"), "false");
  CHECK_STR(eval(f.L, "select('#', m.nothing(1))"), "0");  // void pushes nothing
}

auto test_containers() -> void {
  t::section("containers");
  Fixture f;
  CHECK_STR(eval(f.L, "table.concat(m.scale({1,2,3}, 2.5), ',')"), "2.5,5.0,7.5");
  CHECK_STR(eval(f.L, "table.concat(m.lengths({'a','bb','ccc'}), ',')"), "1,2,3");
  CHECK_STR(eval(f.L, "#m.scale({}, 2)"), "0");
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

}  // namespace

auto main() -> int {
  test_scalar_roundtrip();
  test_containers();
  test_optional();
  test_argument_errors();
  test_no_leak_on_partial_arguments();
  return t::summary();
}
