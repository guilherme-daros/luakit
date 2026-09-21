// Library: a Lua module built purely from free functions, no class involved.
//
//   auto luaopen_luna(luakit::core::State *L) -> int {
//     return luakit::Library(L)
//         .fn<sum>("sum")
//         .fn<greet>("greet")
//         .fn<scale>("scale")
//         .fn<boom>("boom")
//         .build_module();
//   }
//
// This is the free-function half of what Class<T> does for a class: the
// array and its null terminator are folded away into a builder instead of
// kept in the open, and build_module() finishes it exactly the way
// Class<T>::build_module() does.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/function.hpp"
#include "luakit/overload.hpp"

#include <vector>

namespace luakit {

class Library {
 public:
  explicit Library(core::State *L) : L_(L) {}

  Library(const Library &) = delete;
  auto operator=(const Library &) -> Library & = delete;

  // A free function, callable as mod.name(...).
  template <auto F>
  auto fn(const char *name) -> Library & {
    funcs_.push_back({name, luakit::fn<F>});
    return *this;
  }

  // Several free functions under one Lua name, tried in order.
  template <auto... Fs>
  auto overload(const char *name) -> Library & {
    funcs_.push_back({name, fn_overload<Fs...>});
    return *this;
  }

  // A hand-written core::CFunction. A constructor built with
  // luakit::ctor/ctor_overload lands here just as well.
  auto raw_fn(const char *name, core::CFunction f) -> Library & {
    funcs_.push_back({name, f});
    return *this;
  }

  // Builds the module table and pushes it. Returns 1, so a luaopen_ function
  // can return it directly.
  auto build_module() -> int {
    funcs_.push_back({nullptr, nullptr});
    core::aux::newlib(L_, funcs_.data());
    return 1;
  }

 private:
  core::State *L_;
  std::vector<core::aux::Reg> funcs_;
};

}  // namespace luakit
