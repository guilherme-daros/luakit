// Benchmarks for the paths a host actually runs every frame, and for the
// question a host asks when deciding how to structure one: does this loop
// belong in C++ or in Lua.
//
//   cmake -S . -B build -DLUNA_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target bench
//   ./build/bench/bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
//   ./build/bench/bench --benchmark_filter=Sequence --benchmark_counters_tabular=true
//
// One interpreter is built once in main(), below, and reached by every
// benchmark through g_lua: constructing one per benchmark would swamp every
// measurement in setup cost that is not what is being measured.
//
// Where a benchmark drives Lua from C++, setup -- compiling a chunk, reading
// out a Function -- happens before the `for (auto _ : state)` loop, which is
// the boundary google/benchmark actually times; only what is inside it counts.
//
// Two directions are measured, and they are not the same cost. "Lua calls
// C++" is a script reaching for a bound free function, method or field, which
// is what the *_Crossing and *_Inheritance groups below isolate through a
// tiny precompiled closure invoked once per iteration. That closure call is
// itself luakit::Function::call, measured on its own as
// Crossing_FunctionCallRoundTrip; subtract it to see the C++ side alone.
// "C++ calls Lua" is the direction the Sequence group is about.

#include "luakit/callback.hpp"
#include "luakit/class.hpp"
#include "luakit/coroutine.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/library.hpp"
#include "luakit/table.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

luakit::Interpreter *g_lua = nullptr;

// -------------------------------------------------------------- fixtures

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

// The object the Sequence group moves. A plain value the engine owns and
// lends out, the same shape a game's Entity or particle would be.
struct Point {
  double x = 0;
  double y = 0;
  auto step(double dx, double dy) -> Point & {
    x += dx;
    y += dy;
    return *this;
  }
};

A g_a;
B g_b;
D g_d;

constexpr int k_max_points = 10000;
std::vector<Point> g_points(k_max_points);

auto flat() -> A * {
  return &g_a;
}
auto mid() -> B * {
  return &g_b;
}
auto deep_obj() -> D * {
  return &g_d;
}
auto as_base() -> A * {
  return &g_d;
}  // really a D: the resolution path
auto add(double x, double y) -> double {
  return x + y;
}
auto echo(std::string s) -> std::string {
  return s;
}

// Bounds-checked only by the caller passing a sane n; this is a benchmark
// fixture, not part of the library's contract.
auto point_at(int i) -> Point * {
  return &g_points[static_cast<std::size_t>(i)];
}

auto vector_of(int n) -> std::vector<double> {
  return std::vector<double>(static_cast<std::size_t>(n), 1.5);
}
auto map_of(int n) -> std::map<std::string, int> {
  std::map<std::string, int> out;
  for (int i = 0; i < n; ++i) out["k" + std::to_string(i)] = i;
  return out;
}

auto pick_one(int a) -> int {
  return a;
}
auto pick_two(double a) -> int {
  return static_cast<int>(a);
}
auto pick_three(std::string a) -> int {
  return static_cast<int>(a.size());
}
auto pick_four(bool a) -> int {
  return a ? 1 : 0;
}

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
template <>
struct luakit::Metatable<Point> {
  static constexpr const char *k_name = "b.Point";
};

namespace {

auto open_bench(luakit::core::State *L) -> int {
  luakit::Class<A>(L).method<&A::touch>("touch").prop<&A::a>("a").build();
  luakit::Class<B>(L).prop<&B::b>("b").build();
  luakit::Class<C>(L).prop<&C::c>("c").build();
  luakit::Class<D>(L).method<&D::deep>("deep").prop<&D::d>("d").build();
  luakit::Class<Point>(L).method<&Point::step>("step").prop<&Point::x>("x").prop<&Point::y>("y").build();

  return luakit::Library(L)
      .fn<flat>("flat")
      .fn<mid>("mid")
      .fn<deep_obj>("deep_obj")
      .fn<as_base>("as_base")
      .fn<add>("add")
      .fn<echo>("echo")
      .fn<point_at>("point_at")
      .fn<vector_of>("vector_of")
      .fn<map_of>("map_of")
      .overload<pick_one, pick_two, pick_three, pick_four>("pick_1_of_4")
      .fn<pick_one>("pick_1_of_1")
      .build_module();
}

// Compiles a chunk once and hands back the closure it returns, so a
// benchmark's setup cost is "read one Function" rather than "parse Lua".
auto closure(const char *body) -> luakit::Function {
  return g_lua->eval<luakit::Function>(std::string("return function(...) ") + body + " end");
}

// ============================================================ crossings
//
// One call, one field read, one push -- exactly one boundary crossing per
// iteration, driven by a tiny Lua closure invoked from C++. Each closure pays
// one Function::call plus the cost actually being measured; see
// Crossing_FunctionCallRoundTrip below for the part that is not the cost
// being measured.

void Crossing_FunctionCallRoundTrip(benchmark::State &state) {
  auto fn = closure("");
  for (auto _ : state) fn.call<void>();
}
BENCHMARK(Crossing_FunctionCallRoundTrip);

void Crossing_FreeFunctionTwoNumbers(benchmark::State &state) {
  auto fn = closure("return b.add(1, 2)");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<double>());
}
BENCHMARK(Crossing_FreeFunctionTwoNumbers);

void Crossing_FreeFunctionString(benchmark::State &state) {
  auto fn = closure("return b.echo('x')");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<std::string>());
}
BENCHMARK(Crossing_FreeFunctionString);

void Crossing_MethodOnBorrowedObject(benchmark::State &state) {
  auto fn = closure("return a_obj:touch()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Crossing_MethodOnBorrowedObject);

void Crossing_PropertyRead(benchmark::State &state) {
  auto fn = closure("return a_obj.a");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Crossing_PropertyRead);

void Crossing_PropertyWrite(benchmark::State &state) {
  auto fn = closure("a_obj.a = 1");
  for (auto _ : state) fn.call<void>();
}
BENCHMARK(Crossing_PropertyWrite);

// Only exercises the real cost -- a rawget on the object's uservalue table --
// once that table exists, which means something has to be stashed on a_obj
// first. Without that, index_dispatch finds no uservalue table at all and
// skips straight to nil, undercounting what a mod that actually uses this
// pays.
void Crossing_PerInstanceStateRead(benchmark::State &state) {
  g_lua->script("a_obj.stashed = 42");
  auto fn = closure("return a_obj.stashed");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Crossing_PerInstanceStateRead);

void Crossing_TableGet(benchmark::State &state) {
  auto t = g_lua->eval<luakit::Table>("return { width = 1280, title = 'demo' }");
  for (auto _ : state) benchmark::DoNotOptimize(t.get<int>("width"));
}
BENCHMARK(Crossing_TableGet);

void Crossing_TableSet(benchmark::State &state) {
  auto t = luakit::Table::create(g_lua->raw());
  for (auto _ : state) t.set("width", 1280);
}
BENCHMARK(Crossing_TableSet);

// A coroutine that never finishes, so what is measured is one resume, not the
// creation or the eventual completion.
void Crossing_CoroutineResume(benchmark::State &state) {
  g_lua->script("function bench_forever() while true do coroutine.yield() end end");
  auto co = luakit::Coroutine(*g_lua, "bench_forever");
  for (auto _ : state) benchmark::DoNotOptimize(co.resume<void>());
}
BENCHMARK(Crossing_CoroutineResume);

// One arm to try versus four, so the per-arm cost of an overload set is the
// difference between them rather than a guess.
void Crossing_OverloadOneArm(benchmark::State &state) {
  auto fn = closure("return b.pick_1_of_1(1)");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Crossing_OverloadOneArm);

void Crossing_OverloadFourArmsLastMatches(benchmark::State &state) {
  auto fn = closure("return b.pick_1_of_4(true)");  // matches only the fourth arm
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Crossing_OverloadFourArmsLastMatches);

// =========================================================== inheritance
//
// The README says a member lookup costs the same however deep the hierarchy
// is. Measured, that holds for a class's *own* members -- flattening at
// registration is doing its job -- but reaching an *inherited* member costs
// more, because converting the receiver misses the exact-metatable check in
// try_get and goes through the cast table. Depth is free; inheritance is not
// quite.

void Inheritance_OwnMethodNoBases(benchmark::State &state) {
  auto fn = closure("return a_obj:touch()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Inheritance_OwnMethodNoBases);

void Inheritance_OwnMethodFourDeep(benchmark::State &state) {
  auto fn = closure("return d_obj:deep()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Inheritance_OwnMethodFourDeep);

void Inheritance_InheritedMethodOneLevelUp(benchmark::State &state) {
  auto fn = closure("return b_obj:touch()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Inheritance_InheritedMethodOneLevelUp);

void Inheritance_InheritedMethodFourLevelsUp(benchmark::State &state) {
  auto fn = closure("return d_obj:touch()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Inheritance_InheritedMethodFourLevelsUp);

void Inheritance_OwnFieldFourDeep(benchmark::State &state) {
  auto fn = closure("return d_obj.d");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Inheritance_OwnFieldFourDeep);

void Inheritance_InheritedFieldFourLevelsUp(benchmark::State &state) {
  auto fn = closure("return d_obj.a");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>());
}
BENCHMARK(Inheritance_InheritedFieldFourLevelsUp);

// ============================================================ identity
//
// Pushing a base pointer costs a typeid comparison and, on the first push of
// an object, a dynamic_cast, before the identity cache. Both should stay
// small next to the Lua call around them.

void Identity_PushExactType(benchmark::State &state) {
  auto fn = closure("return b.deep_obj()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<luakit::Ref>());
}
BENCHMARK(Identity_PushExactType);

void Identity_PushBaseResolvedToDerived(benchmark::State &state) {
  auto fn = closure("return b.as_base()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<luakit::Ref>());
}
BENCHMARK(Identity_PushBaseResolvedToDerived);

void Identity_PushNonPolymorphic(benchmark::State &state) {
  auto fn = closure("return b.flat()");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<luakit::Ref>());
}
BENCHMARK(Identity_PushNonPolymorphic);

// ========================================================== containers
//
// Marshalling a container costs one Lua table per element, both ways. Sized
// so the fixed per-call cost (visible at n=1) can be told apart from the
// per-element cost (the slope from there to n=1000).

void Containers_VectorDoubleArg(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  auto fn = closure("return #b.vector_of(...)");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>(n));
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Containers_VectorDoubleArg)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

void Containers_MapStringIntArg(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  auto fn = closure("local t = b.map_of(...) local c = 0 for _ in pairs(t) do c = c + 1 end return c");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<int>(n));
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Containers_MapStringIntArg)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================ sequence
//
// The same computation, run four ways, to answer the question a host
// actually has: where should this loop live.
//
//   PureCpp                 no Lua at all -- the floor.
//   CppDrivesLuaPerItem      C++ loops N times; each iteration is a full
//                            round trip into Lua for one point. The shape a
//                            host gets by calling a script hook per entity.
//   LuaDrivesLoop            one round trip into Lua, which loops N times
//                            itself, touching a real bound C++ object each
//                            time. The shape a scripted game frame actually
//                            has: the loop lives where the data announcing
//                            "do the next one" does.
//   LuaOnly                  one round trip into Lua, which loops N times
//                            doing pure Lua arithmetic and never reaches
//                            back into C++. The ceiling: what a computation
//                            costs when it does not need the engine at all.
//
// Reported as items (points) per second, which is what makes the four
// comparable across N: divide CppDrivesLuaPerItem's number into PureCpp's to
// read off the crossing tax directly, and LuaDrivesLoop's into
// CppDrivesLuaPerItem's to see what moving the loop into Lua bought back.

void Sequence_PureCpp(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  for (auto _ : state) {
    for (int i = 0; i < n; ++i) point_at(i)->step(1.0, 1.0);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Sequence_PureCpp)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void Sequence_CppDrivesLuaPerItem(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  auto fn = closure("local i = ... b.point_at(i):step(1.0, 1.0)");
  for (auto _ : state) {
    for (int i = 0; i < n; ++i) fn.call<void>(i);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Sequence_CppDrivesLuaPerItem)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void Sequence_LuaDrivesLoop(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  auto fn = closure("local count = ... for i = 0, count - 1 do b.point_at(i):step(1.0, 1.0) end");
  for (auto _ : state) fn.call<void>(n);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Sequence_LuaDrivesLoop)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

void Sequence_LuaOnly(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  auto fn = closure(
      "local count = ... local x, y = 0, 0 "
      "for i = 1, count do x = x + 1.0 y = y + 1.0 end return x, y");
  for (auto _ : state) benchmark::DoNotOptimize(fn.call<std::tuple<double, double>>(n));
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Sequence_LuaOnly)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

// ============================================================= sandbox
//
// Limits.instructions arms a count hook that fires every k_hook_step VM
// instructions (state.hpp). This is its steady-state tax: the same
// LuaDrivesLoop workload, once with no limit and once with one set high
// enough never to trip, so the only difference is the hook itself.

auto sandboxed_lua() -> luakit::Interpreter & {
  static luakit::Interpreter lua(luakit::Limits{.instructions = 1'000'000'000});
  static bool ready = false;
  if (!ready) {
    lua.openlibs().preload({"b", open_bench});
    lua.script("b = require('b')");
    ready = true;
  }
  return lua;
}

void Sandbox_LuaDrivesLoopNoLimit(benchmark::State &state) {
  const auto n = static_cast<int>(state.range(0));
  auto fn = closure("local count = ... for i = 0, count - 1 do b.point_at(i):step(1.0, 1.0) end");
  for (auto _ : state) fn.call<void>(n);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Sandbox_LuaDrivesLoopNoLimit)->Arg(1000);

void Sandbox_LuaDrivesLoopWithInstructionBudget(benchmark::State &state) {
  luakit::Interpreter &lua = sandboxed_lua();
  const auto n = static_cast<int>(state.range(0));
  auto fn =
      lua.eval<luakit::Function>("return function(count) for i = 0, count - 1 do b.point_at(i):step(1.0, 1.0) end end");
  for (auto _ : state) fn.call<void>(n);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(Sandbox_LuaDrivesLoopWithInstructionBudget)->Arg(1000);

}  // namespace

auto main(int argc, char **argv) -> int {
  luakit::Interpreter lua;
  lua.openlibs().preload({"b", open_bench});
  lua.script("b = require('b') a_obj = b.flat() b_obj = b.mid() d_obj = b.deep_obj()");
  g_lua = &lua;

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
