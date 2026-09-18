// Table: reading and writing Lua tables from C++ without touching the stack.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/error.hpp"
#include "luakit/ref.hpp"
#include "luakit/stack.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace luakit {

namespace detail {

// Renders a key for a diagnostic. lua_tostring answers for strings and
// numbers, which covers every key worth naming; anything else is described by
// its type. Only ever called on an error path, since reading a number this way
// converts the stack slot in place.
inline auto describe_key(core::State *L, int idx) -> std::string {
  const char *s = core::tostring(L, idx);
  if (s) return std::string("'") + s + "'";
  return std::string("of type ") + core::aux::typename_(L, idx);
}

}  // namespace detail

// A Lua table held by C++, so the host can read a plugin's configuration and
// hand data back without writing stack code:
//
//   auto cfg = lua.globals().get<luakit::Table>("config");
//   const int width = cfg.get_or<int>("width", 1280);
//
// Access is raw -- rawget and rawset, not gettable and settable. Two reasons.
// A raw read cannot raise, which is what lets these be ordinary functions
// rather than protected calls; and a host reading a plugin's settings wants
// what the plugin actually wrote, not whatever an __index metamethod would
// prefer it to see. The escape hatch for the other case is push() plus the
// core API.
//
// Move-only, because Ref is.
class Table {
 public:
  Table() = default;

  // A new empty table. The hints size it up front and are a pure optimisation:
  // narr array slots, nrec hash slots.
  static auto create(core::State *L, int narr = 0, int nrec = 0) -> Table {
    core::createtable(L, narr, nrec);
    return Table(Ref::pop(L));
  }

  // Anchors a copy of the table at idx, leaving the caller's stack untouched.
  static auto at(core::State *L, int idx) -> Table { return Table(Ref::at(L, idx)); }

  // Anchors the table on top of the stack, popping it.
  static auto pop(core::State *L) -> Table { return Table(Ref::pop(L)); }

  // The globals table, _G.
  static auto globals(core::State *L) -> Table {
    core::pushglobaltable(L);
    return Table(Ref::pop(L));
  }

  auto valid() const noexcept -> bool { return ref_.valid(); }
  explicit operator bool() const noexcept { return valid(); }

  // Reads a field. Throws if it is absent or not a V, which makes it the right
  // choice for something the host requires and the wrong one for a setting
  // with a default -- use get_or for that.
  template <typename V, typename K>
  auto get(K &&key) const -> V {
    static_assert(!Stack<V>::borrows,
                  "luakit: Table::get cannot return a borrowed type. The value leaves the stack "
                  "before get returns, so a std::string_view or const char * would outlive it. "
                  "Use std::string.");

    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);

    push_field(L, std::forward<K>(key));  // [t, k, v]
    if (!Stack<V>::test(L, -1)) {
      throw Error("luakit: table field " + detail::describe_key(L, -2) + " is a " + core::aux::typename_(L, -1) +
                  " where " + Stack<V>::name + " was expected");
    }
    return Stack<V>::get(L, -1);
  }

  // Reads a field, falling back when it is absent or the wrong type. The
  // wrong-type case falls back too rather than throwing: a plugin author who
  // wrote `width = "big"` gets the default and a working game, which is the
  // behaviour a setting wants.
  template <typename V, typename K>
  auto get_or(K &&key, V fallback) const -> V {
    static_assert(!Stack<V>::borrows, "luakit: Table::get_or cannot return a borrowed type. Use std::string.");

    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);

    push_field(L, std::forward<K>(key));
    if (!Stack<V>::test(L, -1)) return fallback;
    return Stack<V>::get(L, -1);
  }

  // Whether the field holds anything. A field explicitly set to nil counts as
  // absent, which is how Lua itself sees it.
  template <typename K>
  auto has(K &&key) const -> bool {
    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);

    push_field(L, std::forward<K>(key));
    return !core::isnoneornil(L, -1);
  }

  // Writes a field. Returns *this, so writes chain.
  template <typename K, typename V>
  auto set(K &&key, V &&value) -> Table & {
    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);

    luakit::push(L, std::forward<K>(key));
    luakit::push(L, std::forward<V>(value));
    core::rawset(L, -3);
    return *this;
  }

  // Removes a field.
  template <typename K>
  auto erase(K &&key) -> Table & {
    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);

    luakit::push(L, std::forward<K>(key));
    core::pushnil(L);
    core::rawset(L, -3);
    return *this;
  }

  // The sequence length, as the # operator would report it.
  auto length() const -> std::size_t {
    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);
    return core::rawlen(L, -1);
  }

  // The number of entries, counting the hash part that # ignores. Linear.
  auto count() const -> std::size_t {
    core::State *L = begin();
    detail::StackRestore restore(L, core::gettop(L) - 1);

    std::size_t n = 0;
    core::pushnil(L);
    while (core::next(L, -2)) {
      ++n;
      core::pop(L, 1);  // the value; next needs the key left in place
    }
    return n;
  }

  auto push(core::State *L) const -> int { return ref_.push(L); }
  auto state() const noexcept -> core::State * { return ref_.state(); }
  auto reset() noexcept -> void { ref_.reset(); }

 private:
  explicit Table(Ref r) : ref_(std::move(r)) {}

  // Pushes the table and reserves room for a key and a value. Every operation
  // starts this way, and each pairs it with a StackRestore taking the top back
  // to where it was before the table went on.
  auto begin() const -> core::State * {
    core::State *L = ref_.state();
    if (!L) throw Error("luakit: operation on an empty Table");
    if (!core::checkstack(L, 3)) throw Error("luakit: cannot grow the Lua stack");
    ref_.push(L);
    return L;
  }

  // Leaves [table, key, value], keeping the key for diagnostics.
  template <typename K>
  static auto push_field(core::State *L, K &&key) -> void {
    luakit::push(L, std::forward<K>(key));
    core::pushvalue(L, -1);
    core::rawget(L, -3);
  }

  Ref ref_;
};

// A table parameter, so a plugin can hand the host a configuration table and
// have it arrive as something readable.
template <>
struct Stack<Table> {
  static constexpr const char *name = "table";

  // A Table anchors its value in the registry, so it keeps working after the
  // stack slot it came from is gone.
  static constexpr bool borrows = false;

  static auto test(core::State *L, int idx) noexcept -> bool { return core::type(L, idx) == core::TTABLE; }
  static auto check(core::State *L, int idx) -> void { core::aux::checktype(L, idx, core::TTABLE); }

  // On the contract: get() must not raise, and luaL_ref can, by failing to
  // grow the registry. That is the out-of-memory case Userdata<T>::reserve
  // documents; there is no allocation-free way to anchor a value.
  static auto get(core::State *L, int idx) -> Table { return Table::at(L, idx); }

  static auto push(core::State *L, const Table &t) -> int {
    t.push(L);
    return 1;
  }
};

}  // namespace luakit
