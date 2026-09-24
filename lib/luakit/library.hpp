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
#include "luakit/coroutine.hpp"
#include "luakit/doc.hpp"
#include "luakit/enums.hpp"
#include "luakit/function.hpp"
#include "luakit/overload.hpp"

#include <string>
#include <utility>
#include <vector>

namespace luakit {

class Library {
 public:
  // The module name is optional and used only for documentation: it is what
  // luakit::doc::emit_module needs in order to say which require() reaches
  // these functions. Nothing about the registration itself changes.
  explicit Library(core::State *L, const char *module_name = nullptr) : L_(L), name_(module_name ? module_name : "") {}

  Library(const Library &) = delete;
  auto operator=(const Library &) -> Library & = delete;

  // A free function, callable as mod.name(...).
  template <auto F>
  auto fn(const char *name) -> Library & {
    funcs_.push_back({name, luakit::fn<F>});
    members_.push_back({doc::Kind::fn, name, {&doc::describe_fn<F>}});
    return *this;
  }

  // Several free functions under one Lua name, tried in order.
  template <auto... Fs>
  auto overload(const char *name) -> Library & {
    funcs_.push_back({name, fn_overload<Fs...>});
    members_.push_back({doc::Kind::fn, name, {&doc::describe_fn<Fs>...}});
    return *this;
  }

  // A hand-written core::CFunction. A constructor built with
  // luakit::ctor/ctor_overload lands here just as well.
  auto raw_fn(const char *name, core::CFunction f) -> Library & {
    funcs_.push_back({name, f});
    members_.push_back({doc::Kind::fn, name, {}});  // nothing to introspect
    return *this;
  }

  // A function that suspends the coroutine calling it. The same binding
  // raw_fn(name, luakit::yielding<F>) produces, except that the signature is
  // still visible, so it reaches the generated definitions like anything else.
  template <auto F>
  auto yielding_fn(const char *name) -> Library & {
    funcs_.push_back({name, luakit::yielding<F>});
    members_.push_back({doc::Kind::fn, name, {&doc::describe_fn<F>}});
    return *this;
  }

  // A constructor for a class registered elsewhere, living in this module --
  // which is where Lua expects to call it from: `world.vec2(3, 4)`, not
  // `Vec2:new(3, 4)`.
  template <typename T, typename... A>
  auto ctor(const char *name) -> Library & {
    funcs_.push_back({name, luakit::ctor<T, A...>});
    members_.push_back({doc::Kind::ctor, name, {&doc::describe_ctor<T, A...>}});
    return *this;
  }

  template <typename T, typename... Lists>
  auto ctors(const char *name) -> Library & {
    funcs_.push_back({name, ctor_overload<T, Lists...>});
    members_.push_back({doc::Kind::ctor, name, {&doc::CtorOf<T, Lists>::run...}});
    return *this;
  }

  // An enum's names as a table in the module: `world.Facing.north`.
  template <typename E>
  auto enum_(const char *name) -> Library & {
    tables_.push_back({name, &push_enum_table<E>});
    members_.push_back({doc::Kind::enumeration, name, {&doc::describe_enum_table<E>}});
    return *this;
  }

  // Builds the module table and pushes it. Returns 1, so a luaopen_ function
  // can return it directly.
  auto build_module() -> int {
    funcs_.push_back({nullptr, nullptr});
    core::aux::newlib(L_, funcs_.data());

    for (const auto &entry : tables_) {
      entry.second(L_);
      core::setfield(L_, -2, entry.first);
    }

    doc::record(doc::ModuleDoc{name_, members_});
    return 1;
  }

 private:
  core::State *L_;
  std::string name_;
  std::vector<core::aux::Reg> funcs_;
  std::vector<std::pair<const char *, void (*)(core::State *)>> tables_;
  std::vector<doc::Member> members_;
};

}  // namespace luakit
