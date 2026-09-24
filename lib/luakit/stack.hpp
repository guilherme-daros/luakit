#pragma once

#include "luakit/core/api.hpp"
#include "luakit/error.hpp"
#include "luakit/ref.hpp"
#include "luakit/userdata.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luakit {

namespace detail {
template <typename>
inline constexpr bool always_false = false;
}  // namespace detail

// Transfers a C++ value to and from the Lua stack. Specialize for new types.
//
// The four-part contract is what makes this safe. A Lua error is a longjmp and
// skips destructors, so the phases are kept strictly apart:
//
//   name           human-readable type, for diagnostics
//   borrows        true when get() returns a view into Lua-owned memory
//   test(L, idx)   never raises, never allocates; answers "is this a T?"
//   check(L, idx)  may raise a Lua error, but must not create a C++ object
//   get(L, idx)    may create C++ objects (and may throw), but must NEVER
//                  raise a Lua error
//   push(L, v)     returns the number of stack values pushed
//
// Callers run every check first, then every get. A bad argument N is therefore
// reported while no argument 1..N-1 object is alive to be leaked. `test` exists
// so containers can validate elements and still report which element failed.
//
// `borrows` marks the conversions that do not copy: const char *, string_view
// and the registered-class pointers all point into memory the collector owns.
// That is fine for a parameter, which lives only as long as the call, but a
// caller that keeps the value past the point where Lua could collect it needs
// to know. A composite borrows if any part of it does, so the flag propagates
// through optional and vector rather than being restated.
template <typename T>
struct Stack {
  // A class type reaches here when it has no Metatable<T>, which is a far more
  // common mistake than a genuinely unhandled type, so the message names both.
  static_assert(detail::always_false<T>,
                "luakit: no Stack<T> specialization for this type. A class passed to or from Lua "
                "needs a Metatable<T> declaring `static constexpr const char *k_name`; anything "
                "else needs its own Stack<T>.");
};

// bool is excluded so it reaches its own specialization below, which is
// strict about Lua truthiness.
template <std::integral T>
  requires(!std::same_as<T, bool>)
struct Stack<T> {
  static constexpr const char *name = "integer";
  static constexpr bool borrows = false;
  static auto test(core::State *L, int idx) noexcept -> bool {
    int ok = 0;
    core::tointegerx(L, idx, &ok);
    return ok != 0;
  }
  static auto check(core::State *L, int idx) -> void { core::aux::checkinteger(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> T { return static_cast<T>(core::tointeger(L, idx)); }
  static auto push(core::State *L, T v) -> int {
    core::pushinteger(L, static_cast<core::Integer>(v));
    return 1;
  }
};

template <std::floating_point T>
struct Stack<T> {
  static constexpr const char *name = "number";
  static constexpr bool borrows = false;
  static auto test(core::State *L, int idx) noexcept -> bool {
    int ok = 0;
    core::tonumberx(L, idx, &ok);
    return ok != 0;
  }
  static auto check(core::State *L, int idx) -> void { core::aux::checknumber(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> T { return static_cast<T>(core::tonumber(L, idx)); }
  static auto push(core::State *L, T v) -> int {
    core::pushnumber(L, static_cast<core::Number>(v));
    return 1;
  }
};

// Strict: a bool parameter accepts only a boolean, not Lua truthiness. The
// error message is far more useful than silently accepting a table.
template <>
struct Stack<bool> {
  static constexpr const char *name = "boolean";
  static constexpr bool borrows = false;
  static auto test(core::State *L, int idx) noexcept -> bool { return core::type(L, idx) == core::TBOOLEAN; }
  static auto check(core::State *L, int idx) -> void { core::aux::checktype(L, idx, core::TBOOLEAN); }
  static auto get(core::State *L, int idx) noexcept -> bool { return core::toboolean(L, idx) != 0; }
  static auto push(core::State *L, bool v) -> int {
    core::pushboolean(L, v);
    return 1;
  }
};

// Borrowed: the pointer is owned by Lua and stays valid only while the value
// is on the stack and unmodified.
template <>
struct Stack<const char *> {
  static constexpr const char *name = "string";
  static constexpr bool borrows = true;
  static auto test(core::State *L, int idx) noexcept -> bool { return core::isstring(L, idx) != 0; }
  static auto check(core::State *L, int idx) -> void { core::aux::checkstring(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> const char * { return core::tolstring(L, idx, nullptr); }
  static auto push(core::State *L, const char *v) -> int {
    core::pushstring(L, v);
    return 1;
  }
};

// Accepts numbers as well as strings, matching luaL_checkstring. Note that
// reading a number this way converts the stack slot to a string in place,
// which is Lua's documented behaviour.
template <>
struct Stack<std::string> {
  static constexpr const char *name = "string";
  static constexpr bool borrows = false;
  static auto test(core::State *L, int idx) noexcept -> bool { return core::isstring(L, idx) != 0; }
  static auto check(core::State *L, int idx) -> void { core::aux::checkstring(L, idx); }
  static auto get(core::State *L, int idx) -> std::string {
    std::size_t n = 0;
    const char *s = core::tolstring(L, idx, &n);
    return std::string(s, n);  // copies, so embedded NULs survive
  }
  static auto push(core::State *L, const std::string &v) -> int {
    core::pushlstring(L, v.data(), v.size());
    return 1;
  }
};

// Borrowed, like const char*: points into Lua-owned memory.
template <>
struct Stack<std::string_view> {
  static constexpr const char *name = "string";
  static constexpr bool borrows = true;
  static auto test(core::State *L, int idx) noexcept -> bool { return core::isstring(L, idx) != 0; }
  static auto check(core::State *L, int idx) -> void { core::aux::checkstring(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> std::string_view {
    std::size_t n = 0;
    const char *s = core::tolstring(L, idx, &n);
    return std::string_view(s, n);
  }
  static auto push(core::State *L, std::string_view v) -> int {
    core::pushlstring(L, v.data(), v.size());
    return 1;
  }
};

// nil and "no value" both map to nullopt, which is what makes a parameter
// optional at the Lua call site.
template <typename T>
struct Stack<std::optional<T>> {
  static constexpr const char *name = Stack<T>::name;
  static constexpr bool borrows = Stack<T>::borrows;
  static auto test(core::State *L, int idx) noexcept -> bool {
    return core::isnoneornil(L, idx) || Stack<T>::test(L, idx);
  }
  static auto check(core::State *L, int idx) -> void {
    if (!core::isnoneornil(L, idx)) Stack<T>::check(L, idx);
  }
  static auto get(core::State *L, int idx) -> std::optional<T> {
    if (core::isnoneornil(L, idx)) return std::nullopt;
    return Stack<T>::get(L, idx);
  }
  static auto push(core::State *L, const std::optional<T> &v) -> int {
    if (!v) {
      core::pushnil(L);
      return 1;
    }
    return Stack<T>::push(L, *v);
  }
};

// A Lua sequence table. Length and element access are raw: core::rawlen and
// core::rawgeti cannot raise, which get() requires.
template <typename T>
struct Stack<std::vector<T>> {
  static constexpr const char *name = "table";
  static constexpr bool borrows = Stack<T>::borrows;

  static auto test(core::State *L, int idx) noexcept -> bool {
    if (core::type(L, idx) != core::TTABLE) return false;
    const int at = core::absindex(L, idx);
    if (!core::checkstack(L, 2)) return false;
    const auto n = static_cast<core::Integer>(core::rawlen(L, at));
    for (core::Integer i = 1; i <= n; ++i) {
      core::rawgeti(L, at, i);
      const bool ok = Stack<T>::test(L, -1);
      core::pop(L, 1);
      if (!ok) return false;
    }
    return true;
  }

  static auto check(core::State *L, int idx) -> void {
    const int at = core::absindex(L, idx);
    core::aux::checktype(L, at, core::TTABLE);
    core::aux::checkstack(L, 2, "luakit: vector element");

    const auto n = static_cast<core::Integer>(core::rawlen(L, at));
    for (core::Integer i = 1; i <= n; ++i) {
      core::rawgeti(L, at, i);
      const bool ok = Stack<T>::test(L, -1);
      core::pop(L, 1);  // pop before raising, so the stack stays balanced
      if (!ok) {
        core::aux::argerror(L, at, core::pushfstring(L, "element %d is not a %s", static_cast<int>(i), Stack<T>::name));
      }
    }
  }

  static auto get(core::State *L, int idx) -> std::vector<T> {
    const int at = core::absindex(L, idx);
    const auto n = static_cast<core::Integer>(core::rawlen(L, at));

    // core::checkstack reports failure by returning 0 rather than raising, so
    // the vector below can never be skipped over by a longjmp.
    if (!core::checkstack(L, 2)) throw std::runtime_error("luakit: cannot grow Lua stack");

    std::vector<T> out;
    out.reserve(static_cast<std::size_t>(n));
    for (core::Integer i = 1; i <= n; ++i) {
      core::rawgeti(L, at, i);
      out.push_back(Stack<T>::get(L, -1));  // may throw, unwinds normally
      core::pop(L, 1);
    }
    return out;
  }

  static auto push(core::State *L, const std::vector<T> &v) -> int {
    core::createtable(L, static_cast<int>(v.size()), 0);
    const int t = core::gettop(L);
    for (std::size_t i = 0; i < v.size(); ++i) {
      Stack<T>::push(L, v[i]);
      core::rawseti(L, t, static_cast<core::Integer>(i + 1));
    }
    return 1;
  }
};

namespace detail {

// ------------------------------------------------- fixed-length sequences
//
// std::pair, std::array and std::tuple all map to { e1, ..., eN }, and all
// three answer std::tuple_size and std::tuple_element, so one implementation
// covers them.

template <typename E>
auto elem_test(core::State *L, int at, std::size_t i) noexcept -> bool {
  core::rawgeti(L, at, static_cast<core::Integer>(i + 1));
  const bool ok = Stack<E>::test(L, -1);
  core::pop(L, 1);
  return ok;
}

template <typename E>
auto elem_get(core::State *L, int at, std::size_t i) -> E {
  core::rawgeti(L, at, static_cast<core::Integer>(i + 1));
  E out = Stack<E>::get(L, -1);
  core::pop(L, 1);
  return out;
}

template <typename Tup, std::size_t... I>
auto fixed_test(core::State *L, int at, std::index_sequence<I...>) noexcept -> bool {
  return (elem_test<std::tuple_element_t<I, Tup>>(L, at, I) && ...);
}

template <typename Tup, std::size_t... I>
auto fixed_check(core::State *L, int at, std::index_sequence<I...>) -> void {
  (
      [&] {
        if (elem_test<std::tuple_element_t<I, Tup>>(L, at, I)) return;
        core::aux::argerror(L, at,
                            core::pushfstring(L, "element %d is not a %s", static_cast<int>(I) + 1,
                                              Stack<std::tuple_element_t<I, Tup>>::name));
      }(),
      ...);
}

// Braced init is ordered, so elements are read left to right and one that
// throws part way unwinds those already built.
template <typename Tup, std::size_t... I>
auto fixed_get(core::State *L, int at, std::index_sequence<I...>) -> Tup {
  return Tup{elem_get<std::tuple_element_t<I, Tup>>(L, at, I)...};
}

template <typename Tup, std::size_t... I>
auto fixed_push(core::State *L, const Tup &v, std::index_sequence<I...>) -> int {
  core::createtable(L, static_cast<int>(sizeof...(I)), 0);
  const int t = core::gettop(L);
  ((Stack<std::tuple_element_t<I, Tup>>::push(L, std::get<I>(v)),
    core::rawseti(L, t, static_cast<core::Integer>(I) + 1)),
   ...);
  return 1;
}

template <typename Tup>
inline constexpr bool fixed_borrows = []<std::size_t... I>(std::index_sequence<I...>) {
  return (Stack<std::tuple_element_t<I, Tup>>::borrows || ...);
}(std::make_index_sequence<std::tuple_size_v<Tup>>{});

// The shared body of every fixed-length sequence specialization.
template <typename Tup>
struct FixedSequence {
  using seq = std::make_index_sequence<std::tuple_size_v<Tup>>;
  static constexpr int k_len = static_cast<int>(std::tuple_size_v<Tup>);

  static constexpr const char *name = "table";
  static constexpr bool borrows = fixed_borrows<Tup>;

  static auto test(core::State *L, int idx) noexcept -> bool {
    if (core::type(L, idx) != core::TTABLE) return false;
    const int at = core::absindex(L, idx);
    if (!core::checkstack(L, 2)) return false;
    if (static_cast<int>(core::rawlen(L, at)) != k_len) return false;
    return fixed_test<Tup>(L, at, seq{});
  }

  static auto check(core::State *L, int idx) -> void {
    const int at = core::absindex(L, idx);
    core::aux::checktype(L, at, core::TTABLE);
    core::aux::checkstack(L, 2, "luakit: sequence element");

    const int len = static_cast<int>(core::rawlen(L, at));
    if (len != k_len) {
      core::aux::argerror(L, at, core::pushfstring(L, "expected %d elements, got %d", k_len, len));
    }
    fixed_check<Tup>(L, at, seq{});
  }

  static auto get(core::State *L, int idx) -> Tup {
    const int at = core::absindex(L, idx);
    if (!core::checkstack(L, 2)) throw std::runtime_error("luakit: cannot grow Lua stack");
    return fixed_get<Tup>(L, at, seq{});
  }

  static auto push(core::State *L, const Tup &v) -> int { return fixed_push(L, v, seq{}); }
};

// -------------------------------------------------------------- maps
//
// The copy of the key in each loop is not optional. Reading a number key as a
// string converts the stack slot in place, which is documented Lua behaviour
// and also corrupts the traversal: the next lua_next then fails with "invalid
// key to 'next'". So the key is duplicated and the copy is what gets read.

template <typename M>
auto map_test(core::State *L, int idx) noexcept -> bool {
  using K = typename M::key_type;
  using V = typename M::mapped_type;

  if (core::type(L, idx) != core::TTABLE) return false;
  const int at = core::absindex(L, idx);
  if (!core::checkstack(L, 4)) return false;

  core::pushnil(L);
  while (core::next(L, at)) {  // [key, value]
    core::pushvalue(L, -2);    // [key, value, key-copy]
    const bool ok = Stack<K>::test(L, -1) && Stack<V>::test(L, -2);
    core::pop(L, 2);  // [key]
    if (!ok) {
      core::pop(L, 1);  // abandon the traversal
      return false;
    }
  }
  return true;
}

template <typename M>
auto map_check(core::State *L, int idx) -> void {
  using K = typename M::key_type;
  using V = typename M::mapped_type;

  const int at = core::absindex(L, idx);
  core::aux::checktype(L, at, core::TTABLE);
  core::aux::checkstack(L, 4, "luakit: map entry");

  core::pushnil(L);
  while (core::next(L, at)) {
    core::pushvalue(L, -2);
    const bool key_ok = Stack<K>::test(L, -1);
    const bool value_ok = Stack<V>::test(L, -2);

    if (!key_ok || !value_ok) {
      // Nothing with a destructor is alive here: argerror longjmps, and Lua
      // unwinds its own stack on the way out.
      const char *shown = core::tostring(L, -1);
      core::aux::argerror(L, at,
                          core::pushfstring(L, key_ok ? "value at key '%s' is not a %s" : "key '%s' is not a %s",
                                            shown ? shown : "?", key_ok ? Stack<V>::name : Stack<K>::name));
    }
    core::pop(L, 2);
  }
}

template <typename M>
auto map_get(core::State *L, int idx) -> M {
  using K = typename M::key_type;
  using V = typename M::mapped_type;

  const int at = core::absindex(L, idx);
  if (!core::checkstack(L, 4)) throw std::runtime_error("luakit: cannot grow Lua stack");

  M out;
  core::pushnil(L);
  while (core::next(L, at)) {
    core::pushvalue(L, -2);
    K key = Stack<K>::get(L, -1);
    V value = Stack<V>::get(L, -2);
    out.emplace(std::move(key), std::move(value));
    core::pop(L, 2);
  }
  return out;
}

template <typename M>
auto map_push(core::State *L, const M &m) -> int {
  core::createtable(L, 0, static_cast<int>(m.size()));
  const int t = core::gettop(L);
  for (const auto &entry : m) {
    Stack<typename M::key_type>::push(L, entry.first);
    Stack<typename M::mapped_type>::push(L, entry.second);
    core::rawset(L, t);
  }
  return 1;
}

// ---------------------------------------------------------------- sets
//
// A set crosses as a sequence table, { e1, ..., eN }, the same shape a vector
// uses: a mod writing `world.despawn({"rat", "bat"})` should not have to know
// which container the host chose. Duplicates in the table collapse, which is
// what asking for a set means.

template <typename S>
struct SetLike {
  using value_type = typename S::value_type;

  static constexpr const char *name = "table";
  static constexpr bool borrows = Stack<value_type>::borrows;

  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<std::vector<value_type>>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Stack<std::vector<value_type>>::check(L, idx); }

  static auto get(core::State *L, int idx) -> S {
    const int at = core::absindex(L, idx);
    const auto n = static_cast<core::Integer>(core::rawlen(L, at));
    if (!core::checkstack(L, 2)) throw std::runtime_error("luakit: cannot grow Lua stack");

    S out;
    for (core::Integer i = 1; i <= n; ++i) {
      core::rawgeti(L, at, i);
      out.insert(Stack<value_type>::get(L, -1));  // may throw, unwinds normally
      core::pop(L, 1);
    }
    return out;
  }

  static auto push(core::State *L, const S &s) -> int {
    core::createtable(L, static_cast<int>(s.size()), 0);
    const int t = core::gettop(L);
    core::Integer i = 0;
    for (const auto &v : s) {
      Stack<value_type>::push(L, v);
      core::rawseti(L, t, ++i);
    }
    return 1;
  }
};

// The shared body of every map-like specialization.
template <typename M>
struct MapLike {
  static constexpr const char *name = "table";
  static constexpr bool borrows = Stack<typename M::key_type>::borrows || Stack<typename M::mapped_type>::borrows;

  static auto test(core::State *L, int idx) noexcept -> bool { return map_test<M>(L, idx); }
  static auto check(core::State *L, int idx) -> void { map_check<M>(L, idx); }
  static auto get(core::State *L, int idx) -> M { return map_get<M>(L, idx); }
  static auto push(core::State *L, const M &m) -> int { return map_push(L, m); }
};

}  // namespace detail

// A Lua table used as a dictionary, which is what a plugin's settings table
// usually is. Iteration order is Lua's, so the C++ side decides whether it
// wants the keys sorted by choosing map over unordered_map.
template <typename K, typename V, typename C, typename A>
struct Stack<std::map<K, V, C, A>> : detail::MapLike<std::map<K, V, C, A>> {};

template <typename K, typename V, typename H, typename E, typename A>
struct Stack<std::unordered_map<K, V, H, E, A>> : detail::MapLike<std::unordered_map<K, V, H, E, A>> {};

// A set of names or ids, as the sequence table a script would write by hand.
// Iteration order is the container's, so std::set pushes sorted and
// unordered_set does not.
template <typename T, typename C, typename A>
struct Stack<std::set<T, C, A>> : detail::SetLike<std::set<T, C, A>> {};

template <typename T, typename H, typename E, typename A>
struct Stack<std::unordered_set<T, H, E, A>> : detail::SetLike<std::unordered_set<T, H, E, A>> {};

// A two-element sequence table, { first, second }. The readable way to move a
// coordinate or a key-value pair across without inventing a class for it.
template <typename A, typename B>
struct Stack<std::pair<A, B>> : detail::FixedSequence<std::pair<A, B>> {};

// A sequence table of exactly N elements. The length is part of the type, so a
// table of the wrong size is rejected by name rather than silently padded.
template <typename T, std::size_t N>
struct Stack<std::array<T, N>> : detail::FixedSequence<std::array<T, N>> {};

// A tuple as a *value* is a fixed-length sequence table, like pair and array.
//
// A tuple *returned from a bound function* is the one exception: that means
// multiple Lua values, because multiple returns are how Lua says it and there
// is no other spelling for them. The special case lives in
// detail::invoke_and_push and in Function::call, and nowhere else -- a tuple
// parameter, a tuple inside a vector, a tuple written to a table field all go
// through this specialization and are tables.
template <typename... Ts>
struct Stack<std::tuple<Ts...>> : detail::FixedSequence<std::tuple<Ts...>> {};

// Registered classes, by pointer. Pushing lends the object rather than copying
// it: the userdata is marked borrowed, so __gc leaves the host's object alone.
// That is what lets an engine pass a script its own entity.
//
// Borrowed in the Stack<T> sense too, and for the matching reason -- the
// pointer stays good only while the userdata holding it is reachable.
template <Registered T>
struct Stack<T *> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = true;
  static auto test(core::State *L, int idx) noexcept -> bool { return Userdata<T>::try_get(L, idx) != nullptr; }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }
  // Goes through try_get rather than reading the box, because a Derived
  // arriving here needs its pointer adjusted to the base subobject.
  static auto get(core::State *L, int idx) noexcept -> T * { return Userdata<T>::try_get(L, idx); }
  static auto push(core::State *L, T *v) -> int {
    Userdata<T>::push_ref(L, v);
    return 1;
  }
};

template <Registered T>
struct Stack<T &> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = true;
  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> T & { return *Stack<T *>::get(L, idx); }
  static auto push(core::State *L, T &v) -> int {
    Userdata<T>::push_ref(L, &v);
    return 1;
  }
};

// The const spellings of the two above.
//
// Lua has no const, so it is erased at the boundary: a script handed a
// `const Entity *` can call any bound mutator on it. Worth knowing, and the
// price of being able to bind `const Entity *find() const` at all rather than
// dropping const from the host's own API to satisfy the binding layer.
//
// Written out rather than folded into Stack<T *> because the constraint there
// is `Registered T`, and Registered<const Entity> is false: nobody specializes
// Metatable for a const type. So these deduce T without the cv and put it
// back, which also keeps the two partial specializations unambiguous.
template <Registered T>
struct Stack<const T *> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = true;
  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> const T * { return Stack<T *>::get(L, idx); }
  static auto push(core::State *L, const T *v) -> int {
    Userdata<T>::push_ref(L, const_cast<T *>(v));
    return 1;
  }
};

template <Registered T>
struct Stack<const T &> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = true;
  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> const T & { return *Stack<T *>::get(L, idx); }
  static auto push(core::State *L, const T &v) -> int {
    Userdata<T>::push_ref(L, const_cast<T *>(&v));
    return 1;
  }
};

// A registered class by value: Lua gets its own copy, which the collector owns
// and destroys. This is the safe default for a function handing back something
// with no lifetime of its own, where lending a pointer would dangle.
template <Registered T>
struct Stack<T> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = false;
  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }

  static auto get(core::State *L, int idx) -> T {
    static_assert(std::copy_constructible<T>,
                  "luakit: taking this class from Lua by value needs it to be copy-constructible. "
                  "Take it as T & or T * instead, which lends the object rather than copying it.");
    return *Stack<T *>::get(L, idx);
  }

  // Constructed straight into the box, so the copy happens after the raising
  // allocation rather than before it.
  static auto push(core::State *L, const T &v) -> int {
    auto *b = Userdata<T>::reserve(L);
    new (Userdata<T>::storage(b)) T(v);
    Userdata<T>::commit(L, b);
    return 1;
  }

  static auto push(core::State *L, T &&v) -> int {
    auto *b = Userdata<T>::reserve(L);
    new (Userdata<T>::storage(b)) T(std::move(v));
    Userdata<T>::commit(L, b);
    return 1;
  }
};

// A shared_ptr shares ownership with the host, so neither side has to outlive
// the other -- the natural fit for an object a script may keep a handle to
// after the engine has let go of it.
template <Registered T>
struct Stack<std::shared_ptr<T>> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = false;
  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }

  // Only a box that actually holds a shared_ptr can hand its count out. An
  // owned or borrowed object has no count to share, and aliasing one into a
  // fresh shared_ptr would invent an owner that does not exist.
  //
  // The exact type is required too: a Derived box holds a shared_ptr<Derived>,
  // and reading those bytes as a shared_ptr<Base> would be a lie about the
  // layout even where the pointers happen to coincide.
  static auto get(core::State *L, int idx) -> std::shared_ptr<T> {
    void *exact = core::aux::testudata(L, idx, Metatable<T>::k_name);
    if (!exact) return {};
    auto *b = static_cast<Box<T> *>(exact);
    if (b->mode != Ownership::shared) return {};
    return *Userdata<T>::template payload_of<std::shared_ptr<T>>(b);
  }

  static auto push(core::State *L, const std::shared_ptr<T> &v) -> int {
    Userdata<T>::push_shared(L, v);
    return 1;
  }
};

// A unique_ptr hands the object over for good: the host built it, is done with
// it, and the collector destroys it. This is the transfer case, which
// returning T by value cannot express -- an Entity is typically neither
// copyable nor movable, and is already allocated somewhere else.
//
// Push only. Taking a unique_ptr *out* of Lua would mean tearing an object out
// of a box that scripts may still hold handles to, which has no safe answer;
// the diagnostic below says what to write instead.
template <Registered T>
struct Stack<std::unique_ptr<T>> {
  static constexpr const char *name = Metatable<T>::k_name;
  static constexpr bool borrows = false;

  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }

  static auto get(core::State *, int) -> std::unique_ptr<T> {
    static_assert(detail::always_false<T>,
                  "luakit: a unique_ptr parameter would take ownership away from Lua, which may "
                  "still hold handles to the object. Take T & or T * to borrow it, or "
                  "std::shared_ptr<T> to share it.");
    return {};
  }

  static auto push(core::State *L, std::unique_ptr<T> &&v) -> int {
    Userdata<T>::push_unique(L, std::move(v));
    return 1;
  }
};

// Any Lua value at all, anchored so it keeps working after the call.
//
// Table and Function each have one of these; this is the same idea without the
// type requirement, for a handler taking whatever a mod felt like passing:
// `on_event(std::string name, luakit::Ref payload)`.
template <>
struct Stack<Ref> {
  static constexpr const char *name = "any";

  // False, for the same reason Table's and Function's are: the value lives in
  // the registry from here on, not in the stack slot it came from.
  static constexpr bool borrows = false;

  // A missing argument is the one thing this refuses. Everything present is a
  // value, including nil, and anchoring nil is meaningful -- it says the mod
  // passed something and that something was nil.
  static auto test(core::State *L, int idx) noexcept -> bool { return !core::isnone(L, idx); }
  static auto check(core::State *L, int idx) -> void {
    if (core::isnone(L, idx)) core::aux::argerror(L, idx, "value expected");
  }

  // On the contract: get() must not raise, and luaL_ref can, by failing to
  // grow the registry. That is the out-of-memory case Userdata<T>::reserve
  // documents; there is no allocation-free way to anchor a value.
  static auto get(core::State *L, int idx) -> Ref { return Ref::at(L, idx); }

  static auto push(core::State *L, const Ref &r) -> int {
    r.push(L);
    return 1;
  }
};

namespace detail {

// Which Stack<> specialization handles a value of type T. References to
// registered classes stay references, so the object is lent rather than
// copied; everything else decays, so `const std::string &` and `std::string`
// share one specialization.
//
// The cv-qualifier is kept on a registered reference, so `const Vec2 &` maps
// to Stack<const Vec2 &> rather than Stack<Vec2 &>, which is what lets a
// getter return `const Vec2 &`.
template <typename T>
using stack_key_t =
    std::conditional_t<std::is_lvalue_reference_v<T> && Registered<std::remove_cv_t<std::remove_reference_t<T>>>,
                       std::remove_reference_t<T> &, std::decay_t<T>>;

// Converts, or throws. The counterpart to luakit::get below, for host code:
// a failed Stack<T>::check raises a Lua error, and a call from outside a
// protected frame would longjmp straight to the panic handler and abort.
template <typename T>
auto read_result(core::State *L, int idx) -> T {
  if (!Stack<T>::test(L, idx)) {
    throw Error(std::string("luakit: Lua gave a ") + core::aux::typename_(L, idx) + " where " + Stack<T>::name +
                " was expected");
  }
  return Stack<T>::get(L, idx);
}

template <typename>
struct is_tuple : std::false_type {};
template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type {};

// ------------------------------------------------- results of a Lua call
//
// A tuple is the one type that means something different coming *back* from
// Lua than it does going in. As a value it is a fixed-length sequence table,
// like pair and array. As the result type of a call it means multiple Lua
// results, because multiple returns are how Lua says it and there is no other
// spelling available.
//
// These three say so once, and Function::call, Coroutine::resume and
// Interpreter::eval all share them, so one spelling means one thing wherever
// a call's results are read.

// How many Lua values a C++ result type asks for.
template <typename R>
inline constexpr int result_count = 1;
template <>
inline constexpr int result_count<void> = 0;
template <typename... Ts>
inline constexpr int result_count<std::tuple<Ts...>> = static_cast<int>(sizeof...(Ts));

// Whether reading a result would produce a view into the Lua stack. A tuple
// borrows if any of its elements does.
template <typename R>
inline constexpr bool result_borrows = Stack<R>::borrows;
template <>
inline constexpr bool result_borrows<void> = false;
template <typename... Ts>
inline constexpr bool result_borrows<std::tuple<Ts...>> = (Stack<Ts>::borrows || ...);

// Reads result_count<R> values sitting at base+1 upwards.
//
// Braced init is ordered, so elements are read left to right and a mismatch
// part way through unwinds the ones already built.
template <typename R>
auto read_results(core::State *L, int base) -> R {
  if constexpr (std::is_void_v<R>) {
    return;
  } else if constexpr (is_tuple<R>::value) {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return R{read_result<std::tuple_element_t<I, R>>(L, base + 1 + static_cast<int>(I))...};
    }(std::make_index_sequence<std::tuple_size_v<R>>{});
  } else {
    return read_result<R>(L, base + 1);
  }
}

}  // namespace detail

// Convenience wrappers for hand-written code running under a protected call:
// get() reports a bad value by raising, the way a bound function should.
template <typename T>
auto get(core::State *L, int idx) -> T {
  Stack<T>::check(L, idx);
  return Stack<T>::get(L, idx);
}

template <typename T>
auto push(core::State *L, T &&v) -> int {
  return Stack<detail::stack_key_t<T>>::push(L, std::forward<T>(v));
}

}  // namespace luakit
