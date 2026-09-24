// The type shapes an engine API produces: const pointers and references,
// unique_ptr transfer, any-value Refs, sets, and a T & return that is not the
// receiver.

#include "check.hpp"

#include "luakit/class.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/library.hpp"
#include "luakit/ref.hpp"

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int g_widget_dtors = 0;

struct Widget {
  std::string name;
  int hp = 10;

  explicit Widget(std::string n) : name(std::move(n)) {}
  ~Widget() { ++g_widget_dtors; }

  Widget(const Widget &) = delete;
  auto operator=(const Widget &) -> Widget & = delete;
};

// A container whose children are Widgets it owns, so `child()` returns a T &
// that is not the receiver.
struct Rack {
  std::vector<std::unique_ptr<Widget>> parts;

  Rack() {
    parts.push_back(std::make_unique<Widget>("left"));
    parts.push_back(std::make_unique<Widget>("right"));
  }

  auto child(int i) -> Widget & { return *parts.at(static_cast<std::size_t>(i - 1)); }
  auto self() -> Rack & { return *this; }
  auto count() const -> int { return static_cast<int>(parts.size()); }
};

Rack g_rack;

auto peek(const Widget *w) -> std::string {
  return w->name;
}
auto peek_ref(const Widget &w) -> int {
  return w.hp;
}
auto first_const() -> const Widget * {
  return &g_rack.child(1);
}
auto make_widget(std::string name) -> std::unique_ptr<Widget> {
  return std::make_unique<Widget>(std::move(name));
}
auto the_rack() -> Rack * {
  return &g_rack;
}

auto sorted(std::set<std::string> in) -> std::vector<std::string> {
  return {in.begin(), in.end()};
}
auto unique_count(std::unordered_set<int> in) -> int {
  return static_cast<int>(in.size());
}

// Keeps whatever it was handed, whatever that was.
luakit::Ref g_kept;
auto keep(luakit::Ref value) -> void {
  g_kept = std::move(value);
}
auto kept() -> luakit::Ref {
  return std::move(g_kept);
}

}  // namespace

template <>
struct luakit::Metatable<Widget> {
  static constexpr const char *k_name = "test.Widget";
};
template <>
struct luakit::Metatable<Rack> {
  static constexpr const char *k_name = "test.Rack";
};

namespace {

auto open_parts(luakit::core::State *L) -> int {
  luakit::Class<Widget>(L).prop<&Widget::hp>("hp").ro_prop<&Widget::name>("name").build();
  luakit::Class<Rack>(L)
      .method<&Rack::child>("child")
      .method<&Rack::self>("self")
      .method<&Rack::count>("count")
      .build();

  return luakit::Library(L)
      .fn<peek>("peek")
      .fn<peek_ref>("peek_ref")
      .fn<first_const>("first_const")
      .fn<make_widget>("make_widget")
      .fn<the_rack>("the_rack")
      .fn<sorted>("sorted")
      .fn<unique_count>("unique_count")
      .fn<keep>("keep")
      .fn<kept>("kept")
      .build_module();
}

}  // namespace

auto main() -> int {
  {
    luakit::Interpreter lua;
    lua.openlibs().preload({"parts", open_parts});
    lua.script("p = require('parts')");

    t::section("a const pointer and a const reference cross both ways");
    CHECK_STR(lua.eval<std::string>("return p.peek(p.first_const())"), "left");
    CHECK_EQ(lua.eval<int>("return p.peek_ref(p.first_const())"), 10);
    CHECK_STR(lua.eval<std::string>("return p.first_const().name"), "left");

    t::section("a const pointer is the same object as the non-const one");
    CHECK_EQ(lua.eval<bool>("return p.first_const() == p.the_rack():child(1)"), true);

    t::section("a method returning a different T & lends it rather than refusing");
    CHECK_STR(lua.eval<std::string>("return p.the_rack():child(2).name"), "right");
    CHECK_EQ(lua.eval<bool>("return p.the_rack():child(1) ~= p.the_rack():child(2)"), true);

    t::section("returning the receiver still chains");
    CHECK_EQ(lua.eval<bool>("return p.the_rack():self() == p.the_rack()"), true);
    CHECK_EQ(lua.eval<int>("return p.the_rack():self():count()"), 2);

    t::section("a unique_ptr hands the object over and Lua destroys it");
    g_widget_dtors = 0;
    lua.script("local w = p.make_widget('given away') assert(w.name == 'given away') w = nil");
    lua.gc().collect();
    CHECK_EQ(g_widget_dtors, 1);

    t::section("a transferred object behaves like any other");
    CHECK_EQ(lua.eval<int>("local w = p.make_widget('x') w.hp = 3 return w.hp"), 3);
    CHECK_STR(lua.eval<std::string>("return p.peek(p.make_widget('borrowed back'))"), "borrowed back");

    t::section("sets cross as sequence tables");
    CHECK_STR(lua.eval<std::string>("return table.concat(p.sorted({'c', 'a', 'b', 'a'}), ',')"), "a,b,c");
    CHECK_EQ(lua.eval<int>("return p.unique_count({1, 2, 2, 3, 3, 3})"), 3);

    t::section("a Ref carries any value at all, and keeps it");
    CHECK_EQ(lua.eval<int>("p.keep(42) return p.kept()"), 42);
    CHECK_STR(lua.eval<std::string>("p.keep('hello') return p.kept()"), "hello");
    CHECK_EQ(lua.eval<bool>("p.keep({a = 1}) return p.kept().a == 1"), true);
    CHECK_EQ(lua.eval<bool>("p.keep(nil) return p.kept() == nil"), true);
    CHECK_EQ(lua.eval<bool>("local w = p.make_widget('r') p.keep(w) return p.kept() == w"), true);

    t::section("a missing argument is the one thing a Ref refuses");
    CHECK(lua.eval<std::string>("local ok, err = pcall(p.keep) return tostring(err)").find("value expected") !=
          std::string::npos);

    // Released before the interpreter, since it anchors a value in it.
    g_kept.reset();
  }

  t::section("an overload set names its candidates when nothing matches");
  {
    luakit::Interpreter lua;
    lua.openlibs().preload({"parts", open_parts});
    lua.script("p = require('parts')");
    const auto err = lua.eval<std::string>("local ok, e = pcall(p.peek, 1, 2, 3) return tostring(e)");
    CHECK(!err.empty());
  }

  return t::summary();
}
