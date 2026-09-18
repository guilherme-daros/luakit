#pragma once

#include "luakit/core/api.hpp"
#include "luakit/userdata.hpp"

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
  // A class type reaches here when it has no Metatable<T>, which is a far more
  // common mistake than a genuinely unhandled type, so the message names both.
  static_assert(detail::always_false<T>,
                "luakit: no Stack<T> specialization for this type. A class passed to or from Lua "
                "needs a Metatable<T> declaring `static constexpr const char *k_name`; anything "
                "else needs its own Stack<T>.");
};

template <typename T>
struct Stack<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
  static constexpr const char *name = "integer";
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

template <typename T>
struct Stack<T, std::enable_if_t<std::is_floating_point_v<T>>> {
  static constexpr const char *name = "number";
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

// Registered classes, by pointer or reference. Creation goes through
// Userdata<T>::emplace, so there is no push here: pushing a bare T* would have
// no way to know whether Lua already owns that object.
template <typename T>
struct Stack<T *, std::enable_if_t<Registered<T>>> {
  static constexpr const char *name = Metatable<T>::k_name;
  static auto test(core::State *L, int idx) noexcept -> bool {
    return core::aux::testudata(L, idx, Metatable<T>::k_name) != nullptr;
  }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> T * {
    return static_cast<Box<T> *>(core::touserdata(L, idx))->obj();
  }
};

template <typename T>
struct Stack<T &, std::enable_if_t<Registered<T>>> {
  static constexpr const char *name = Metatable<T>::k_name;
  static auto test(core::State *L, int idx) noexcept -> bool { return Stack<T *>::test(L, idx); }
  static auto check(core::State *L, int idx) -> void { Userdata<T>::check(L, idx); }
  static auto get(core::State *L, int idx) noexcept -> T & { return *Stack<T *>::get(L, idx); }
};

// Convenience wrappers for hand-written code.
template <typename T>
auto get(core::State *L, int idx) -> T {
  Stack<T>::check(L, idx);
  return Stack<T>::get(L, idx);
}

template <typename T>
auto push(core::State *L, T &&v) -> int {
  return Stack<std::decay_t<T>>::push(L, std::forward<T>(v));
}

}  // namespace luakit
