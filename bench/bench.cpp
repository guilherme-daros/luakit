// Microbenchmarks for the paths a host actually runs every frame.
//
// Two claims in particular are worth measuring rather than asserting.
//
// The README says a member lookup costs the same however deep the hierarchy
// is. Measured, that holds for a class's *own* members -- flattening at
// registration is doing its job -- but reaching an *inherited* member costs
// roughly twice as much at every depth, because converting the receiver misses
// the exact-metatable check and goes through the cast table. Depth is free;
// inheritance is not quite.
//
// And pushing a base pointer now does a typeid comparison and possibly a
// dynamic_cast before the identity cache, which should stay small next to the
// Lua call around it.
//
//   cmake -S . -B build -DLUNA_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target bench && ./build/bench/bench

#include "luakit/class.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/library.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

struct A {
  virtual ~A() = default;
  int a = 1;
  auto touch() -> int { return ++a; }
};
struct B : A {
  int b = 2;
};
struct C : B {
  int c = 3;
};
struct D : C {
  int d = 4;
  auto deep() -> int { return ++d; }
};

A g_a;
B g_b;
D g_d;

auto flat() -> A * { return &g_a; }
auto mid() -> B * { return &g_b; }
auto deep() -> D * { return &g_d; }
auto as_base() -> A * { return &g_d; }  // really a D: the resolution path
auto add(double x, double y) -> double { return x + y; }
auto echo(std::string s) -> std::string { return s; }

}  // namespace

template <>
struct luakit::Metatable<A> {
  static constexpr const char *k_name = "b.A";
};
template <>
struct luakit::Metatable<B> {
  static constexpr const char *k_name = "b.B";
  using bases = luakit::Bases<A>;
};
template <>
struct luakit::Metatable<C> {
  static constexpr const char *k_name = "b.C";
  using bases = luakit::Bases<B>;
};
template <>
struct luakit::Metatable<D> {
  static constexpr const char *k_name = "b.D";
  using bases = luakit::Bases<C>;
};

namespace {

auto open_bench(luakit::core::State *L) -> int {
  luakit::Class<A>(L).method<&A::touch>("touch").prop<&A::a>("a").build();
  luakit::Class<B>(L).prop<&B::b>("b").build();
  luakit::Class<C>(L).prop<&C::c>("c").build();
  luakit::Class<D>(L).method<&D::deep>("deep").prop<&D::d>("d").build();

  return luakit::Library(L)
      .fn<flat>("flat")
      .fn<mid>("mid")
      .fn<deep>("deep")
      .fn<as_base>("as_base")
      .fn<add>("add")
      .fn<echo>("echo")
      .build_module();
}

// Runs a chunk n times and reports nanoseconds per iteration. The loop lives
// in Lua so that what is measured is the crossing, not the driving.
auto time_it(luakit::Interpreter &lua, const char *label, const char *body, int n = 2000000) -> void {
  const std::string chunk = "local n = " + std::to_string(n) + "\nfor i = 1, n do " + body + " end";

  lua.script(chunk);  // warm up: first run compiles and fills the caches

  const auto start = std::chrono::steady_clock::now();
  lua.script(chunk);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  std::printf("  %-42s %7.1f ns/op\n", label, static_cast<double>(ns) / n);
}

}  // namespace

auto main() -> int {
  luakit::Interpreter lua;
  lua.openlibs().preload({"b", open_bench});
  lua.script("b = require('b') a_obj = b.flat() b_obj = b.mid() d_obj = b.deep()");

  std::printf("\ncalls\n");
  time_it(lua, "free function, two numbers", "b.add(1, 2)");
  time_it(lua, "free function, a string", "b.echo('x')");
  time_it(lua, "method on a borrowed object", "a_obj:touch()");

  std::printf("\nfields\n");
  time_it(lua, "property read", "local _ = a_obj.a");
  time_it(lua, "property write", "a_obj.a = 1");
  time_it(lua, "per-instance state read", "local _ = a_obj.stashed");

  // Members are flattened into each class at registration, so *finding* one is
  // a single rawget whatever the depth. What is not free is converting the
  // receiver: a method declared on A, called on a D, misses the exact-name
  // check in Userdata<A>::try_get and goes through the cast table. That costs
  // the same for any depth, but it is not nothing, and the numbers below are
  // the honest version of "lookup costs the same however deep".
  std::printf("\ninheritance: depth is free, an inherited receiver is not\n");
  time_it(lua, "own method, no bases", "a_obj:touch()");
  time_it(lua, "own method, four deep", "d_obj:deep()");
  time_it(lua, "inherited method, one level up", "b_obj:touch()");
  time_it(lua, "inherited method, four levels up", "d_obj:touch()");
  time_it(lua, "own field, four deep", "local _ = d_obj.d");
  time_it(lua, "inherited field, one level up", "local _ = b_obj.a");
  time_it(lua, "inherited field, four levels up", "local _ = d_obj.a");

  std::printf("\npushing a pointer (identity cache, then resolution)\n");
  time_it(lua, "exact type", "b.deep()");
  time_it(lua, "base pointer, resolved to the derived type", "b.as_base()");
  time_it(lua, "base pointer, non-polymorphic path", "b.flat()");

  std::printf("\n");
  return 0;
}
