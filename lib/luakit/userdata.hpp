#pragma once

#include "luakit/core/api.hpp"

#include <new>
#include <utility>

namespace luakit {

template <typename T>
struct Metatable;

// core::newuserdatauv returns storage the collector already tracks, before the
// constructor has run. `live` lets __gc tell a built object from raw memory,
// and makes double finalization a no-op.
template <typename T>
struct Box {
  bool live;
  alignas(T) unsigned char storage[sizeof(T)];

  auto obj() noexcept -> T * { return reinterpret_cast<T *>(storage); }
};

template <typename T>
struct Userdata {
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
