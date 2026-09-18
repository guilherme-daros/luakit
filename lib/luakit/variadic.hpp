// Variadic: a bound function whose argument count Lua decides.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/error.hpp"
#include "luakit/stack.hpp"

#include <string>

namespace luakit {

// The arguments past the ones a bound function named, for the cases where a
// fixed signature will not do -- a log() that takes whatever it is handed, a
// spawn() that accepts any number of components:
//
//   auto log(std::string level, luakit::Variadic rest) -> void {
//     for (int i = 0; i < rest.size(); ++i) line += rest.to_string(i);
//   }
//
// Must be the last parameter, which detail::free_call enforces: everything
// after it would have no arguments left to read.
//
// A view over live stack slots, so it is only good for the duration of the
// call. Stack<Variadic>::borrows says so, which is what stops it being used as
// a return type or kept in a Function result.
class Variadic {
 public:
  Variadic() = default;
  Variadic(core::State *L, int first) noexcept : L_(L), first_(first), count_(core::gettop(L) - first + 1) {
    if (count_ < 0) count_ = 0;
  }

  auto size() const noexcept -> int { return count_; }
  auto empty() const noexcept -> bool { return count_ == 0; }

  // Whether argument i would convert to T. Zero-based, unlike Lua.
  template <typename T>
  auto is(int i) const noexcept -> bool {
    if (!in_range(i)) return false;
    return Stack<T>::test(L_, index(i));
  }

  // Argument i as a T, or a Lua error naming the argument if it is not one.
  // Raising is right here: a Variadic only exists inside a bound call, which
  // is always running under a protected frame.
  template <typename T>
  auto get(int i) const -> T {
    if (!in_range(i)) {
      core::aux::error(L_, "luakit: argument %d of %d requested from a Variadic", i + 1, count_);
    }
    return luakit::get<T>(L_, index(i));
  }

  // Argument i as a T, or the fallback when it is absent or another type.
  template <typename T>
  auto get_or(int i, T fallback) const -> T {
    if (!is<T>(i)) return fallback;
    return Stack<T>::get(L_, index(i));
  }

  // Argument i rendered for a human, whatever it is. Goes through luaL_tolstring,
  // so a value with __tostring speaks for itself.
  auto to_string(int i) const -> std::string {
    if (!in_range(i)) return {};
    core::aux::tolstring(L_, index(i), nullptr);
    std::size_t n = 0;
    const char *s = core::tolstring(L_, -1, &n);
    std::string out(s, n);
    core::pop(L_, 1);
    return out;
  }

  // The name Lua gives argument i's type.
  auto type_name(int i) const -> const char * {
    if (!in_range(i)) return "no value";
    return core::aux::typename_(L_, index(i));
  }

  // The absolute stack index of argument i, for hand-written code.
  auto index(int i) const noexcept -> int { return first_ + i; }

  auto state() const noexcept -> core::State * { return L_; }

 private:
  auto in_range(int i) const noexcept -> bool { return L_ && i >= 0 && i < count_; }

  core::State *L_ = nullptr;
  int first_ = 0;
  int count_ = 0;
};

// Unlike every other specialization this one spans more than a single slot: it
// reads from idx to the top of the stack. That only works because a Variadic
// is required to be the last parameter, so there is nothing after it whose
// index the extra slots would disturb.
template <>
struct Stack<Variadic> {
  static constexpr const char *name = "any";

  // A window onto the caller's stack, so it must not outlive the call.
  static constexpr bool borrows = true;

  // Anything at all, including nothing at all.
  static auto test(core::State *, int) noexcept -> bool { return true; }
  static auto check(core::State *, int) -> void {}
  static auto get(core::State *L, int idx) noexcept -> Variadic { return Variadic(L, core::absindex(L, idx)); }
};

}  // namespace luakit
