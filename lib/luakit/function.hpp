#pragma once

#include "luakit/guard.hpp"
#include "luakit/stack.hpp"
#include "luakit/userdata.hpp"
#include "luakit/variadic.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace luakit {

namespace detail {

template <typename>
struct signature;

template <typename R, typename... A>
struct signature<R (*)(A...)> {
  using ret = R;
  using args = std::tuple<A...>;
};

template <typename R, typename... A>
struct signature<R (*)(A...) noexcept> : signature<R (*)(A...)> {};

template <typename R, typename C, typename... A>
struct signature<R (C::*)(A...)> {
  using ret = R;
  using cls = C;
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct signature<R (C::*)(A...) const> : signature<R (C::*)(A...)> {};

template <typename R, typename C, typename... A>
struct signature<R (C::*)(A...) noexcept> : signature<R (C::*)(A...)> {};

template <typename R, typename C, typename... A>
struct signature<R (C::*)(A...) const noexcept> : signature<R (C::*)(A...)> {};

// A Variadic reads from its own index to the top of the stack, so a parameter
// after one would find its arguments already swallowed. Nothing about that is
// detectable at the call site, which is why it is rejected at the bind.
//
// A struct rather than three separate assertions, because a static_assert
// message has to be a literal and three copies of one would drift apart.
template <typename... A>
struct RequireVariadicLast {
  static constexpr bool value = [] {
    constexpr bool flags[] = {std::is_same_v<stack_key_t<A>, Variadic>..., false};
    for (std::size_t i = 0; i + 1 < sizeof...(A); ++i) {
      if (flags[i]) return false;
    }
    return true;
  }();

  static_assert(value,
                "luakit: a Variadic parameter must come last. It reads every remaining argument, "
                "so a parameter after it would have none left to read.");
};

// Hands a materialised argument to the callable. The tuple is used exactly
// once, so an argument that is only movable -- a luakit::Function holding a
// registry anchor, say -- can be moved out rather than copied. Forwarding as
// stack_key_t rather than blanket-moving is what keeps a registered class
// arriving as the T& it was stored as: std::move would turn that into a T&&
// and stop binding.
template <typename A, typename T>
auto forward_arg(T &v) -> stack_key_t<A> && {
  return std::forward<stack_key_t<A>>(v);
}

template <typename>
struct is_tuple : std::false_type {};
template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type {};

template <typename... Ts>
auto push_all(core::State *L, const std::tuple<Ts...> &t) -> int {
  int n = 0;
  std::apply([&](const auto &...xs) { ((n += Stack<std::decay_t<decltype(xs)>>::push(L, xs)), ...); }, t);
  return n;
}

// Pushes whatever the bound callable returned, and reports how many Lua values
// that was.
template <typename R, typename Invoke>
auto invoke_and_push(core::State *L, Invoke &&invoke) -> int {
  if constexpr (std::is_void_v<R>) {
    invoke();
    return 0;
  } else if constexpr (is_tuple<std::decay_t<R>>::value) {
    return push_all(L, invoke());
  } else {
    return Stack<stack_key_t<R>>::push(L, invoke());
  }
}

template <auto F, typename R, typename... A, std::size_t... I>
auto free_call(core::State *L, std::tuple<A...> *, std::index_sequence<I...>) -> int {
  static_cast<void>(RequireVariadicLast<A...>::value);

  // Phase 1: validate every argument. A fold expression is ordered, so the
  // lowest bad argument is the one reported, and no C++ object exists yet.
  (Stack<stack_key_t<A>>::check(L, static_cast<int>(I) + 1), ...);

  // Phase 2: materialise. Braced init is ordered too, and none of these can
  // raise a Lua error, so the tuple can never be longjmp'd over.
  std::tuple<stack_key_t<A>...> args{Stack<stack_key_t<A>>::get(L, static_cast<int>(I) + 1)...};

  return invoke_and_push<R>(L, [&] { return F(forward_arg<A>(std::get<I>(args))...); });
}

template <auto F>
auto free_impl(core::State *L) -> int {
  using sig = signature<decltype(F)>;
  using args = typename sig::args;
  return free_call<F, typename sig::ret>(L, static_cast<args *>(nullptr),
                                         std::make_index_sequence<std::tuple_size_v<args>>{});
}

template <auto M, typename R, typename C, typename... A, std::size_t... I>
auto method_call(core::State *L, std::tuple<A...> *, std::index_sequence<I...>) -> int {
  static_cast<void>(RequireVariadicLast<A...>::value);

  Userdata<C>::check(L, 1);  // validates self, raises on a foreign or dead object
  (Stack<stack_key_t<A>>::check(L, static_cast<int>(I) + 2), ...);

  C *self = Stack<C *>::get(L, 1);
  std::tuple<stack_key_t<A>...> args{Stack<stack_key_t<A>>::get(L, static_cast<int>(I) + 2)...};

  // A method returning C& is the chaining idiom: hand back the receiver that
  // is already on the stack rather than trying to re-wrap it.
  if constexpr (std::is_lvalue_reference_v<R> && std::is_same_v<std::remove_cv_t<std::remove_reference_t<R>>, C>) {
    C *returned = &(self->*M)(forward_arg<A>(std::get<I>(args))...);
    if (returned != self) {
      core::aux::error(L, "luakit: method returned a different object than self");
    }
    core::settop(L, 1);
    return 1;
  } else {
    return invoke_and_push<R>(L, [&] { return (self->*M)(forward_arg<A>(std::get<I>(args))...); });
  }
}

template <typename T, typename... A, std::size_t... I>
auto ctor_call(core::State *L, std::index_sequence<I...>) -> int {
  static_cast<void>(RequireVariadicLast<A...>::value);

  (Stack<stack_key_t<A>>::check(L, static_cast<int>(I) + 1), ...);

  // Storage is reserved before any argument is materialised, so the raising
  // allocation happens while nothing with a destructor is alive. If the
  // constructor then throws, live is still false and __gc skips the wreckage.
  auto *b = Userdata<T>::reserve(L);

  std::tuple<stack_key_t<A>...> args{Stack<stack_key_t<A>>::get(L, static_cast<int>(I) + 1)...};
  new (Userdata<T>::storage(b)) T(forward_arg<A>(std::get<I>(args))...);
  Userdata<T>::commit(L, b);

  return 1;  // the userdata reserve() pushed
}

template <typename T, typename... Args>
auto ctor_impl(core::State *L) -> int {
  return ctor_call<T, Args...>(L, std::make_index_sequence<sizeof...(Args)>{});
}

template <auto M>
auto method_impl(core::State *L) -> int {
  using sig = signature<decltype(M)>;
  using args = typename sig::args;
  return method_call<M, typename sig::ret, typename sig::cls>(L, static_cast<args *>(nullptr),
                                                              std::make_index_sequence<std::tuple_size_v<args>>{});
}

}  // namespace detail

// Bind a free function. The Lua signature is deduced from the C++ one, so
//
//   auto sum(double a, double b) -> double;
//   {"sum", luakit::fn<sum>}
//
// gets argument checking, conversion and return handling for free.
template <auto F>
inline constexpr core::CFunction fn = &guard<&detail::free_impl<F>>;

// Bind a member function. Argument 1 is the receiver; user arguments start at
// index 2. A C& return value means "self", for chaining.
template <auto M>
inline constexpr core::CFunction method = &guard<&detail::method_impl<M>>;

// Bind a constructor: {"new", luakit::ctor<Tracker, std::string>}.
//
// Unlike a hand-written creator calling Userdata<T>::emplace, this is already
// inside guard<>, so a throwing constructor becomes a Lua error instead of
// std::terminate.
template <typename T, typename... Args>
inline constexpr core::CFunction ctor = &guard<&detail::ctor_impl<T, Args...>>;

}  // namespace luakit
