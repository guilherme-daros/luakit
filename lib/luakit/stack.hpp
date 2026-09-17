#pragma once

#include "luakit/api.hpp"
#include "luakit/binding.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace luakit {

namespace detail {
template <typename>
inline constexpr bool always_false = false;

template <typename T, typename = void>
struct has_metatable : std::false_type {};
template <typename T>
struct has_metatable<T, std::void_t<decltype(Metatable<T>::k_name)>> : std::true_type {};
template <typename T>
inline constexpr bool has_metatable_v = has_metatable<T>::value;
}  // namespace detail

// Transfers a C++ value to and from the Lua stack. Specialize for new types.
//
// The four-part contract is what makes this safe. A Lua error is a longjmp and
// skips destructors, so the phases are kept strictly apart:
//
//   name           human-readable type, for diagnostics
//   test(L, idx)   never raises, never allocates; answers "is this a T?"
//   check(L, idx)  may raise a Lua error, but must not create a C++ object
//   get(L, idx)    may create C++ objects (and may throw), but must NEVER
//                  raise a Lua error
//   push(L, v)     returns the number of stack values pushed
//
// Callers run every check first, then every get. A bad argument N is therefore
// reported while no argument 1..N-1 object is alive to be leaked. `test` exists
// so containers can validate elements and still report which element failed.
template <typename T, typename = void>
struct Stack {
  static_assert(detail::always_false<T>, "luakit: no Stack<T> specialization for this type");
};

template <typename T>
struct Stack<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
  static constexpr const char *name = "integer";
  static auto test(lua::State *L, int idx) noexcept -> bool {
    int ok = 0;
    lua::tointegerx(L, idx, &ok);
    return ok != 0;
  }
  static auto check(lua::State *L, int idx) -> void { lua::aux::checkinteger(L, idx); }
  static auto get(lua::State *L, int idx) noexcept -> T { return static_cast<T>(lua::tointeger(L, idx)); }
  static auto push(lua::State *L, T v) -> int {
    lua::pushinteger(L, static_cast<lua::Integer>(v));
    return 1;
  }
};

template <typename T>
struct Stack<T, std::enable_if_t<std::is_floating_point_v<T>>> {
  static constexpr const char *name = "number";
  static auto test(lua::State *L, int idx) noexcept -> bool {
    int ok = 0;
    lua::tonumberx(L, idx, &ok);
    return ok != 0;
  }
  static auto check(lua::State *L, int idx) -> void { lua::aux::checknumber(L, idx); }
  static auto get(lua::State *L, int idx) noexcept -> T { return static_cast<T>(lua::tonumber(L, idx)); }
  static auto push(lua::State *L, T v) -> int {
    lua::pushnumber(L, static_cast<lua::Number>(v));
    return 1;
  }
};

// Strict: a bool parameter accepts only a boolean, not Lua truthiness. The
// error message is far more useful than silently accepting a table.
template <>
struct Stack<bool> {
  static constexpr const char *name = "boolean";
  static auto test(lua::State *L, int idx) noexcept -> bool { return lua::type(L, idx) == lua::TBOOLEAN; }
  static auto check(lua::State *L, int idx) -> void { lua::aux::checktype(L, idx, lua::TBOOLEAN); }
  static auto get(lua::State *L, int idx) noexcept -> bool { return lua::toboolean(L, idx) != 0; }
  static auto push(lua::State *L, bool v) -> int {
    lua::pushboolean(L, v);
    return 1;
  }
};

// Borrowed: the pointer is owned by Lua and stays valid only while the value
// is on the stack and unmodified.
template <>
struct Stack<const char *> {
  static constexpr const char *name = "string";
  static auto test(lua::State *L, int idx) noexcept -> bool { return lua::isstring(L, idx) != 0; }
  static auto check(lua::State *L, int idx) -> void { lua::aux::checkstring(L, idx); }
  static auto get(lua::State *L, int idx) noexcept -> const char * { return lua::tolstring(L, idx, nullptr); }
  static auto push(lua::State *L, const char *v) -> int {
    lua::pushstring(L, v);
    return 1;
  }
};

// Accepts numbers as well as strings, matching luaL_checkstring. Note that
// reading a number this way converts the stack slot to a string in place,
// which is Lua's documented behaviour.
template <>
struct Stack<std::string> {
  static constexpr const char *name = "string";
  static auto test(lua::State *L, int idx) noexcept -> bool { return lua::isstring(L, idx) != 0; }
  static auto check(lua::State *L, int idx) -> void { lua::aux::checkstring(L, idx); }
  static auto get(lua::State *L, int idx) -> std::string {
    std::size_t n = 0;
    const char *s = lua::tolstring(L, idx, &n);
    return std::string(s, n);  // copies, so embedded NULs survive
  }
  static auto push(lua::State *L, const std::string &v) -> int {
    lua::pushlstring(L, v.data(), v.size());
    return 1;
  }
};

// Borrowed, like const char*: points into Lua-owned memory.
template <>
struct Stack<std::string_view> {
  static constexpr const char *name = "string";
  static auto test(lua::State *L, int idx) noexcept -> bool { return lua::isstring(L, idx) != 0; }
  static auto check(lua::State *L, int idx) -> void { lua::aux::checkstring(L, idx); }
  static auto get(lua::State *L, int idx) noexcept -> std::string_view {
    std::size_t n = 0;
    const char *s = lua::tolstring(L, idx, &n);
    return std::string_view(s, n);
  }
  static auto push(lua::State *L, std::string_view v) -> int {
    lua::pushlstring(L, v.data(), v.size());
    return 1;
  }
};

// nil and "no value" both map to nullopt, which is what makes a parameter
// optional at the Lua call site.
template <typename T>
struct Stack<std::optional<T>> {
  static constexpr const char *name = Stack<T>::name;
  static auto test(lua::State *L, int idx) noexcept -> bool {
    return lua::isnoneornil(L, idx) || Stack<T>::test(L, idx);
  }
  static auto check(lua::State *L, int idx) -> void {
    if (!lua::isnoneornil(L, idx)) Stack<T>::check(L, idx);
  }
  static auto get(lua::State *L, int idx) -> std::optional<T> {
    if (lua::isnoneornil(L, idx)) return std::nullopt;
    return Stack<T>::get(L, idx);
  }
  static auto push(lua::State *L, const std::optional<T> &v) -> int {
    if (!v) {
      lua::pushnil(L);
      return 1;
    }
    return Stack<T>::push(L, *v);
  }
};

// A Lua sequence table. Length and element access are raw: lua::rawlen and
// lua::rawgeti cannot raise, which get() requires.
template <typename T>
struct Stack<std::vector<T>> {
  static constexpr const char *name = "table";

  static auto test(lua::State *L, int idx) noexcept -> bool {
    if (lua::type(L, idx) != lua::TTABLE) return false;
    const int at = lua::absindex(L, idx);
    if (!lua::checkstack(L, 2)) return false;
    const auto n = static_cast<lua::Integer>(lua::rawlen(L, at));
    for (lua::Integer i = 1; i <= n; ++i) {
      lua::rawgeti(L, at, i);
      const bool ok = Stack<T>::test(L, -1);
      lua::pop(L, 1);
      if (!ok) return false;
    }
    return true;
  }

  static auto check(lua::State *L, int idx) -> void {
    const int at = lua::absindex(L, idx);
    lua::aux::checktype(L, at, lua::TTABLE);
    lua::aux::checkstack(L, 2, "luakit: vector element");

    const auto n = static_cast<lua::Integer>(lua::rawlen(L, at));
    for (lua::Integer i = 1; i <= n; ++i) {
      lua::rawgeti(L, at, i);
      const bool ok = Stack<T>::test(L, -1);
      lua::pop(L, 1);  // pop before raising, so the stack stays balanced
      if (!ok) {
        lua::aux::argerror(L, at, lua::pushfstring(L, "element %d is not a %s", static_cast<int>(i), Stack<T>::name));
      }
    }
  }

  static auto get(lua::State *L, int idx) -> std::vector<T> {
    const int at = lua::absindex(L, idx);
    const auto n = static_cast<lua::Integer>(lua::rawlen(L, at));

    // lua::checkstack reports failure by returning 0 rather than raising, so
    // the vector below can never be skipped over by a longjmp.
    if (!lua::checkstack(L, 2)) throw std::runtime_error("luakit: cannot grow Lua stack");

    std::vector<T> out;
    out.reserve(static_cast<std::size_t>(n));
    for (lua::Integer i = 1; i <= n; ++i) {
      lua::rawgeti(L, at, i);
      out.push_back(Stack<T>::get(L, -1));  // may throw, unwinds normally
      lua::pop(L, 1);
    }
    return out;
  }

  static auto push(lua::State *L, const std::vector<T> &v) -> int {
    lua::createtable(L, static_cast<int>(v.size()), 0);
    const int t = lua::gettop(L);
    for (std::size_t i = 0; i < v.size(); ++i) {
      Stack<T>::push(L, v[i]);
      lua::rawseti(L, t, static_cast<lua::Integer>(i + 1));
    }
    return 1;
  }
};

// Registered classes, by pointer or reference. Creation goes through
// Binding<T>::emplace, so there is no push here: pushing a bare T* would have
// no way to know whether Lua already owns that object.
template <typename T>
struct Stack<T *, std::enable_if_t<detail::has_metatable_v<T>>> {
  static constexpr const char *name = Metatable<T>::k_name;
  static auto test(lua::State *L, int idx) noexcept -> bool {
    return lua::aux::testudata(L, idx, Metatable<T>::k_name) != nullptr;
  }
  static auto check(lua::State *L, int idx) -> void { Binding<T>::arg(L, idx); }
  static auto get(lua::State *L, int idx) noexcept -> T * {
    return static_cast<Box<T> *>(lua::touserdata(L, idx))->obj();
  }
};

template <typename T>
struct Stack<T &, std::enable_if_t<detail::has_metatable_v<T>>> {
  static constexpr const char *name = Metatable<T>::k_name;
  static auto test(lua::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(lua::State *L, int idx) -> void { Binding<T>::arg(L, idx); }
  static auto get(lua::State *L, int idx) noexcept -> T & { return *Stack<T *>::get(L, idx); }
};

// Convenience wrappers for hand-written code.
template <typename T>
auto get(lua::State *L, int idx) -> T {
  Stack<T>::check(L, idx);
  return Stack<T>::get(L, idx);
}

template <typename T>
auto push(lua::State *L, T &&v) -> int {
  return Stack<std::decay_t<T>>::push(L, std::forward<T>(v));
}

}  // namespace luakit
