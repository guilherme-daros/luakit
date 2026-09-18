// Ref: anchoring a Lua value in the registry so C++ can outlive the stack.
//
// Run under ASan. The collection case is the one that justifies the class: an
// unanchored value is freed and every later use reads freed memory.

#include "check.hpp"

#include "luakit/interpreter.hpp"
#include "luakit/ref.hpp"

#include <string>
#include <utility>

namespace core = luakit::core;

namespace {

using luakit::Interpreter;
using luakit::Ref;

// Counts live registry entries. luaL_unref recycles slots through a free list
// threaded into the registry itself, so a released slot keeps its key and only
// its value changes -- which is why callers below compare against a baseline
// taken after one full cycle rather than against a fresh interpreter.
auto registry_size(Interpreter &lua) -> int {
  lua.script("__n = 0 for _ in pairs(debug.getregistry()) do __n = __n + 1 end");
  core::getglobal(lua.raw(), "__n");
  const int n = luakit::get<int>(lua.raw(), -1);
  core::pop(lua.raw(), 1);
  return n;
}

auto test_roundtrip() -> void {
  t::section("a value survives the round trip through the registry");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  core::pushstring(L, "anchored");
  Ref r = Ref::pop(L);
  CHECK(r.valid());
  CHECK_EQ(core::gettop(L), 0);  // pop really popped

  CHECK_EQ(r.push(L), core::TSTRING);
  CHECK_STR(luakit::get<std::string>(L, -1), "anchored");
  core::pop(L, 1);
}

auto test_at_leaves_the_stack_alone() -> void {
  t::section("at() copies rather than consuming");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  core::pushinteger(L, 7);
  const int top = core::gettop(L);

  Ref r = Ref::at(L, -1);
  CHECK_EQ(core::gettop(L), top);

  core::pop(L, 1);
  CHECK_EQ(r.push(L), core::TNUMBER);
  CHECK_EQ(luakit::get<int>(L, -1), 7);
  core::pop(L, 1);
}

// Without the registry anchor these collections free the table, and pushing it
// afterwards reads memory the collector has reused.
auto test_survives_collection() -> void {
  t::section("the value survives garbage collection");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  lua.script("__t = {mark = 'still here'}");
  core::getglobal(L, "__t");
  Ref r = Ref::at(L, -1);
  core::pop(L, 1);

  // Drop the only other reference, then collect hard.
  lua.script("__t = nil");
  for (int i = 0; i < 20; ++i) lua.script("collectgarbage('collect')");

  CHECK_EQ(r.push(L), core::TTABLE);
  core::getfield(L, -1, "mark");
  CHECK_STR(luakit::get<std::string>(L, -1), "still here");
  core::pop(L, 2);
}

auto test_empty_ref() -> void {
  t::section("a default-constructed Ref is empty and pushes nil");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  Ref r;
  CHECK(!r.valid());
  CHECK(!static_cast<bool>(r));
  CHECK(r.state() == nullptr);
  CHECK_EQ(r.push(L), core::TNIL);
  core::pop(L, 1);

  r.reset();  // idempotent on an already-empty Ref
  CHECK(!r.valid());
}

// luaL_ref answers a nil value with REFNIL instead of allocating a slot, so
// this path never reaches rawgeti.
auto test_ref_to_nil() -> void {
  t::section("a Ref to nil is valid and pushes nil");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  core::pushnil(L);
  Ref r = Ref::pop(L);
  CHECK(r.valid());
  CHECK_EQ(r.push(L), core::TNIL);
  core::pop(L, 1);
}

auto test_move_semantics() -> void {
  t::section("move transfers the anchor and empties the source");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  core::pushstring(L, "moved");
  Ref a = Ref::pop(L);

  Ref b(std::move(a));
  CHECK(!a.valid());
  CHECK(b.valid());
  CHECK_EQ(b.push(L), core::TSTRING);
  CHECK_STR(luakit::get<std::string>(L, -1), "moved");
  core::pop(L, 1);

  // Assignment must release c's own anchor rather than leak it.
  core::pushstring(L, "replaced");
  Ref c = Ref::pop(L);
  c = std::move(b);
  CHECK(!b.valid());
  CHECK_EQ(c.push(L), core::TSTRING);
  CHECK_STR(luakit::get<std::string>(L, -1), "moved");
  core::pop(L, 1);
}

auto test_reset_releases() -> void {
  t::section("reset releases the anchor and empties the Ref");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  core::pushstring(L, "x");
  Ref r = Ref::pop(L);
  CHECK(r.valid());
  r.reset();
  CHECK(!r.valid());
  CHECK_EQ(r.push(L), core::TNIL);
  core::pop(L, 1);
}

// Every Ref takes a registry slot; the destructor must give it back, or a
// per-frame callback would grow the registry without bound.
auto test_no_registry_leak() -> void {
  t::section("destruction returns the registry slot");
  Interpreter lua;
  lua.open_libs();
  core::State *L = lua.raw();

  // One cycle first: the very first ref is what allocates the free-list head,
  // so a baseline taken before it would be measuring that, not a leak.
  {
    core::pushstring(L, "warmup");
    Ref warmup = Ref::pop(L);
  }

  const int before = registry_size(lua);
  for (int i = 0; i < 200; ++i) {
    core::pushinteger(L, i);
    Ref r = Ref::pop(L);
    CHECK_EQ(r.push(L), core::TNUMBER);
    core::pop(L, 1);
  }
  CHECK_EQ(registry_size(lua), before);
}

}  // namespace

auto main() -> int {
  test_roundtrip();
  test_at_leaves_the_stack_alone();
  test_survives_collection();
  test_empty_ref();
  test_ref_to_nil();
  test_move_semantics();
  test_reset_releases();
  test_no_registry_leak();
  return t::summary();
}
