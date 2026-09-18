// Class<T>: describing a class to Lua in one expression.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/function.hpp"
#include "luakit/overload.hpp"
#include "luakit/property.hpp"
#include "luakit/userdata.hpp"

#include <vector>

namespace luakit {

// The same registration the Reg arrays do, without the arrays:
//
//   auto luaopen_world(luakit::core::State *L) -> int {
//     return luakit::Class<Entity>(L)
//         .method<&Entity::heal>("heal")
//         .prop<&Entity::hp>("hp")
//         .ro_prop<&Entity::name>("name")
//         .accessor<&Entity::get_speed, &Entity::set_speed>("speed")
//         .meta<&Entity::describe>("__tostring")
//         .ctor<std::string>("new")
//         .build_module();
//   }
//
// The arrays still work and are not going anywhere. This exists because three
// parallel ones -- methods, metamethods, properties -- each with its own
// null terminator, is a lot of shape to keep aligned by hand once a class has
// more than a handful of members.
//
// Nothing is registered until build() or build_module() runs, so the builder
// owns its entries up to that point. Names must outlive it, which string
// literals do.
template <typename T>
class Class {
 public:
  explicit Class(core::State *L) : L_(L) {}

  Class(const Class &) = delete;
  auto operator=(const Class &) -> Class & = delete;

  // A member function, callable as obj:name(...).
  template <auto M>
  auto method(const char *name) -> Class & {
    methods_.push_back({name, luakit::method<M>});
    return *this;
  }

  // Several member functions under one name, tried in order.
  template <auto... Ms>
  auto overload(const char *name) -> Class & {
    methods_.push_back({name, method_overload<Ms...>});
    return *this;
  }

  // A hand-written core::CFunction as a method.
  auto raw_method(const char *name, core::CFunction f) -> Class & {
    methods_.push_back({name, f});
    return *this;
  }

  // A metamethod: "__tostring", "__len", "__eq" and friends. These inherit
  // like methods do, so a base's __tostring is what a derived class prints
  // with unless it names its own.
  //
  // "__gc", "__index" and "__newindex" are not available. luakit sets all
  // three last and they win over anything named here: the first destroys the
  // object, and the other two are the field dispatch.
  template <auto M>
  auto meta(const char *name) -> Class & {
    meta_.push_back({name, luakit::method<M>});
    return *this;
  }

  auto raw_meta(const char *name, core::CFunction f) -> Class & {
    meta_.push_back({name, f});
    return *this;
  }

  // A data member, as a readable and writable field.
  template <auto M>
  auto prop(const char *name) -> Class & {
    props_.push_back(luakit::prop<M>(name));
    return *this;
  }

  template <auto M>
  auto ro_prop(const char *name) -> Class & {
    props_.push_back(luakit::ro_prop<M>(name));
    return *this;
  }

  // A computed field, read-only or read-write.
  template <auto Get>
  auto accessor(const char *name) -> Class & {
    props_.push_back(luakit::accessor<Get>(name));
    return *this;
  }

  template <auto Get, auto Set>
  auto accessor(const char *name) -> Class & {
    props_.push_back(luakit::accessor<Get, Set>(name));
    return *this;
  }

  // A constructor, which goes in the module table rather than the metatable:
  // Lua calls it as mod.new(...), not obj:new(...).
  template <typename... A>
  auto ctor(const char *name = "new") -> Class & {
    module_.push_back({name, luakit::ctor<T, A...>});
    return *this;
  }

  // Several constructors under one name.
  //
  //   .ctors<luakit::Args<>, luakit::Args<double, double>>("new")
  template <typename... Lists>
  auto ctors(const char *name = "new") -> Class & {
    module_.push_back({name, ctor_overload<T, Lists...>});
    return *this;
  }

  // A free function alongside the constructors in the module table.
  template <auto F>
  auto fn(const char *name) -> Class & {
    module_.push_back({name, luakit::fn<F>});
    return *this;
  }

  auto raw_fn(const char *name, core::CFunction f) -> Class & {
    module_.push_back({name, f});
    return *this;
  }

  // Registers the class. Pushes nothing.
  auto build() -> void {
    terminate();
    Userdata<T>::register_class(L_, methods_.data(), meta_.empty() ? nullptr : meta_.data(),
                                props_.empty() ? nullptr : props_.data());
  }

  // Registers the class and pushes the module table holding whatever ctor()
  // and fn() named. Returns 1, so a luaopen_ function can return it directly.
  auto build_module() -> int {
    build();
    core::aux::newlib(L_, module_.data());
    return 1;
  }

 private:
  // The Reg arrays are NUL-terminated by convention, and luaL_setfuncs walks
  // until it finds that entry rather than being told a count.
  auto terminate() -> void {
    methods_.push_back({nullptr, nullptr});
    module_.push_back({nullptr, nullptr});
    if (!meta_.empty()) meta_.push_back({nullptr, nullptr});
    if (!props_.empty()) props_.push_back(prop_end);
  }

  core::State *L_;
  std::vector<core::aux::Reg> methods_;
  std::vector<core::aux::Reg> meta_;
  std::vector<core::aux::Reg> module_;
  std::vector<PropertyReg> props_;
};

}  // namespace luakit
