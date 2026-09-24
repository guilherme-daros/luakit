// Overload sets: one Lua name, several C++ signatures.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/function.hpp"
#include "luakit/guard.hpp"
#include "luakit/stack.hpp"
#include "luakit/userdata.hpp"

#include <cstddef>
#include <tuple>
#include <utility>

namespace luakit {

// An argument list, for naming a constructor signature where there is no
// function pointer to deduce one from.
template <typename... Ts>
struct Args {};

namespace detail {

// Whether the arguments starting at `first` match A..., using nothing but the
// test phase of the Stack contract -- which is exactly what test exists for:
// it answers "is this a T?" without raising and without building anything.
template <typename... A, std::size_t... I>
auto args_match(core::State *L, int first, std::index_sequence<I...>) noexcept -> bool {
  // Too many arguments is a mismatch as much as the wrong types are.
  if (core::gettop(L) > first - 1 + static_cast<int>(sizeof...(A))) return false;
  return (Stack<stack_key_t<A>>::test(L, first + static_cast<int>(I)) && ...);
}

// Renders a signature as "(number, string)" for a diagnostic, built on the Lua
// stack so that nothing with a destructor is alive when the error longjmps.
//
// An empty pack gives "()", which is the right thing for a no-argument arm.
template <typename... A>
auto describe_args(core::State *L) -> void {
  core::pushliteral(L, "(");
  int pieces = 1;

  (
      [&] {
        if (pieces > 1) {
          core::pushliteral(L, ", ");
          ++pieces;
        }
        core::pushstring(L, Stack<stack_key_t<A>>::name);
        ++pieces;
      }(),
      ...);

  core::pushliteral(L, ")");
  core::concat(L, pieces + 1);
}

// One candidate in a set. `matches` decides, `call` does the work, `describe`
// says what it wanted when nothing matched; all three are generated from the
// same signature, so they cannot drift apart.
struct Arm {
  bool (*matches)(core::State *L) noexcept;
  int (*call)(core::State *L);
  void (*describe)(core::State *L);
};

template <typename... A>
auto free_matches(core::State *L) noexcept -> bool {
  return args_match<A...>(L, 1, std::make_index_sequence<sizeof...(A)>{});
}

template <auto F, typename... A>
constexpr auto free_arm_for(std::tuple<A...> *) -> Arm {
  return {&free_matches<A...>, &free_impl<F>, &describe_args<A...>};
}

template <auto F>
constexpr auto free_arm() -> Arm {
  using args = typename signature<decltype(F)>::args;
  return free_arm_for<F>(static_cast<args *>(nullptr));
}

// A method's receiver is argument 1, so its own arguments start at 2. The
// receiver is checked by the arm that wins, not by the matcher: every arm in a
// method overload set shares it.
template <typename C, typename... A>
auto method_matches(core::State *L) noexcept -> bool {
  if (!Stack<C *>::test(L, 1)) return false;
  return args_match<A...>(L, 2, std::make_index_sequence<sizeof...(A)>{});
}

template <auto M, typename C, typename... A>
constexpr auto method_arm_for(std::tuple<A...> *) -> Arm {
  return {&method_matches<C, A...>, &method_impl<M>, &describe_args<A...>};
}

template <auto M>
constexpr auto method_arm() -> Arm {
  using sig = signature<decltype(M)>;
  return method_arm_for<M, typename sig::cls>(static_cast<typename sig::args *>(nullptr));
}

template <typename T, typename... A>
constexpr auto ctor_arm() -> Arm {
  return {&free_matches<A...>, &ctor_impl<T, A...>, &describe_args<A...>};
}

template <typename T, typename... A>
constexpr auto ctor_arm_for(Args<A...> *) -> Arm {
  return ctor_arm<T, A...>();
}

// Tries each arm in declaration order and runs the first that fits. Order
// matters and is the author's to choose: a later arm taking std::optional or
// a broader type would otherwise swallow calls meant for an earlier one.
template <Arm... Arms>
auto overload_impl(core::State *L) -> int {
  int result = 0;
  const bool handled = ((Arms.matches(L) ? (result = Arms.call(L), true) : false) || ...);
  if (handled) return result;

  // Nothing matched. With an overload set there is no single argument at
  // fault, so "bad argument #1" would be a guess; the candidates are what the
  // caller actually needs in order to see which one they nearly wrote.
  const int given = core::gettop(L);
  core::pushfstring(L, "luakit: no overload matches the %d argument(s) given. Candidates: ", given);

  int pieces = 1;
  (
      [&] {
        if (pieces > 1) {
          core::pushliteral(L, ", ");
          ++pieces;
        }
        Arms.describe(L);
        ++pieces;
      }(),
      ...);

  core::concat(L, pieces);
  return core::error(L);
}

}  // namespace detail

// Several free functions under one Lua name:
//
//   {"length", luakit::fn_overload<length_2d, length_3d>}
//
// Arms are tried in order, so put the more specific signature first.
template <auto... Fs>
inline constexpr core::CFunction fn_overload = &guard<&detail::overload_impl<detail::free_arm<Fs>()...>>;

// Several member functions under one Lua name. Every arm must belong to the
// same class, since Lua reaches all of them through one receiver.
template <auto... Ms>
inline constexpr core::CFunction method_overload = &guard<&detail::overload_impl<detail::method_arm<Ms>()...>>;

// Several constructors under one Lua name. Spelled with Args<> because there
// is no function pointer to deduce a signature from:
//
//   {"new", luakit::ctor_overload<Vec, luakit::Args<>, luakit::Args<double, double>>}
template <typename T, typename... Lists>
inline constexpr core::CFunction ctor_overload =
    &guard<&detail::overload_impl<detail::ctor_arm_for<T>(static_cast<Lists *>(nullptr))...>>;

}  // namespace luakit
