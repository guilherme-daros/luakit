// Userdata<T> and Box<T>: object lifetime in Lua-managed memory.
//
// These are the tests that justify luakit existing. Run under ASan/LSan: the
// throwing-constructor case double-frees without Box::live, and only a
// sanitizer can see it.

#include "fixture.hpp"

using tf::contains;
using tf::eval;
using tf::Fixture;
using tf::run;

namespace {

// The core invariant: a constructor that throws must leave no object for __gc
// to destroy, and must not double-free what the partially-built constructor
// already unwound.
auto test_throwing_constructor() -> void {
  t::section("throwing constructor leaves no half-built userdata");
  tf::thrower_dtors = 0;
  {
    Fixture f;
    CHECK(contains(eval(f.L, "m.thrower('a-name-too-long-for-SSO-so-it-allocates')"), "constructor failed"));
    run(f.L, "collectgarbage() collectgarbage()");
  }
  CHECK_EQ(tf::thrower_dtors, 0);  // never constructed, so never destroyed
}

auto test_destruction_exactly_once() -> void {
  t::section("each object is destroyed exactly once");
  tf::counter_dtors = 0;
  {
    Fixture f;
    run(f.L, "for i = 1, 20 do local c = m.counter() c:bump(i) end");
    run(f.L, "collectgarbage() collectgarbage()");
    CHECK_EQ(tf::counter_dtors, 20);
    run(f.L, "g = m.counter()");  // still reachable at close
  }
  CHECK_EQ(tf::counter_dtors, 21);  // lua_close finalizes the survivor
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
  test_throwing_constructor();
  test_destruction_exactly_once();
  test_identity();
  return t::summary();
}
