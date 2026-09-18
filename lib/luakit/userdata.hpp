#pragma once

#include "luakit/core/api.hpp"

#include <concepts>
#include <new>
#include <type_traits>
#include <utility>

namespace luakit {

// The name Lua knows T by. Every class exposed as userdata specializes this:
//
//   template <> struct luakit::Metatable<Widget> {
//     static constexpr const char *k_name = "app.Widget";
//   };
template <typename T>
struct Metatable;

// What a usable specialization has to provide. This cannot be a constraint on
// the declaration above: a constraint there would have to name Metatable<T>,
// which is the very name being declared. So the requirement is stated here
// and enforced where a metatable is consumed -- Userdata<T> below, and the
// Stack<T *> / Stack<T &> specializations.
//
// Anything that converts to const char * is accepted, so `static constexpr
// char k_name[]` works as well as a pointer. The second requirement is not
// redundant: Stack<T>::name copies k_name into a constexpr member, so a
// k_name that is merely const would satisfy the conversion here and then fail
// much later, and much less legibly, as "not usable in a constant expression".
template <typename T>
concept Registered = requires {
  { Metatable<T>::k_name } -> std::convertible_to<const char *>;
  typename std::bool_constant<Metatable<T>::k_name != nullptr>;
};

// core::newuserdatauv returns storage the collector already tracks, before the
// constructor has run. `live` lets __gc tell a built object from raw memory,
// and makes double finalization a no-op.
template <typename T>
struct Box {
  bool live;
  alignas(T) unsigned char storage[sizeof(T)];

  auto obj() noexcept -> T * { return reinterpret_cast<T *>(storage); }
};

// Checked with static_assert rather than a requires-clause on the template.
// Constraining Userdata<T> would make the class vanish when T is unregistered,
// and every use of it downstream then fails on its own, burying the one error
// that matters under a page of unrelated ones.
template <typename T>
struct Userdata {
  static_assert(Registered<T>,
                "luakit: Metatable<T> must declare k_name as `static constexpr const char *`. "
                "Write, at namespace scope:\n"
                "  template <> struct luakit::Metatable<YourType> {\n"
                "    static constexpr const char *k_name = \"your.Name\";\n"
                "  };");

  using box_type = Box<T>;

  // Pushes GC-managed storage with the metatable attached, marked not-yet-
  // constructed. The caller must placement-new into box->storage and only then
  // set box->live. Splitting this out lets a caller allocate *before* it
  // materialises any C++ argument, so nothing can be longjmp'd over.
  static auto reserve(core::State *L) -> box_type * {
    void *p = core::newuserdatauv(L, sizeof(box_type), 0);
    auto *b = static_cast<box_type *>(p);
    b->live = false;
    core::aux::setmetatable(L, Metatable<T>::k_name);
    return b;
  }

  // Takes a callable rather than constructor arguments: arguments are
  // evaluated at the call site, which would leave a C++ temporary alive across
  // core::newuserdatauv, and that longjmps on OOM.
  //
  // This can throw, so a hand-written core::CFunction calling it must be
  // wrapped in guard<>. Prefer luakit::ctor<T, Args...>, which is already.
  template <typename F>
  static auto emplace(core::State *L, F &&make) -> T * {
    box_type *b = reserve(L);
    new (b->storage) T(make());
    b->live = true;
    return b->obj();
  }

  // The T at stack index idx, or a Lua error if it is not one.
  static auto check(core::State *L, int idx) -> T * {
    auto *b = static_cast<box_type *>(core::aux::checkudata(L, idx, Metatable<T>::k_name));
    if (!b->live) core::aux::error(L, "%s used after finalization", Metatable<T>::k_name);
    return b->obj();
  }

  // A finalizer must never raise.
  static auto gc(core::State *L) noexcept -> int {
    auto *b = static_cast<box_type *>(core::aux::checkudata(L, 1, Metatable<T>::k_name));
    if (b->live) {
      b->live = false;
      b->obj()->~T();
    }
    return 0;
  }

  static auto register_class(core::State *L, const core::aux::Reg *methods, const core::aux::Reg *meta = nullptr)
      -> void {
    core::aux::newmetatable(L, Metatable<T>::k_name);
    core::pushcfunction(L, gc);
    core::setfield(L, -2, "__gc");
    if (meta) core::aux::setfuncs(L, meta, 0);

    core::aux::newlib(L, methods);
    core::setfield(L, -2, "__index");

    core::pop(L, 1);
  }
};

}  // namespace luakit
