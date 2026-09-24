// Class<T>: describing a class to Lua in one expression.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/doc.hpp"
#include "luakit/enums.hpp"
#include "luakit/function.hpp"
#include "luakit/overload.hpp"
#include "luakit/property.hpp"
#include "luakit/userdata.hpp"

#include <string>
#include <utility>
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
  // The module name is optional and is only ever used for documentation: it
  // says which `require` a mod author reaches this class through, which is
  // something no part of the registration otherwise knows. Passing it is what
  // makes luakit::doc::emit_module able to name the file it is writing.
  explicit Class(core::State *L, const char *module_name = nullptr) : L_(L), module_(module_name ? module_name : "") {}

  Class(const Class &) = delete;
  auto operator=(const Class &) -> Class & = delete;

  // A member function, callable as obj:name(...).
  template <auto M>
  auto method(const char *name) -> Class & {
    methods_.push_back({name, luakit::method<M>});
    members_.push_back({doc::Kind::method, name, {&doc::describe_fn<M>}});
    return *this;
  }

  // Several member functions under one name, tried in order.
  template <auto... Ms>
  auto overload(const char *name) -> Class & {
    methods_.push_back({name, method_overload<Ms...>});
    members_.push_back({doc::Kind::method, name, {&doc::describe_fn<Ms>...}});
    return *this;
  }

  // A hand-written core::CFunction as a method.
  auto raw_method(const char *name, core::CFunction f) -> Class & {
    methods_.push_back({name, f});
    members_.push_back({doc::Kind::method, name, {}});  // nothing to introspect
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
    members_.push_back({doc::Kind::field, name, {&doc::describe_prop<M>}});
    return *this;
  }

  template <auto M>
  auto ro_prop(const char *name) -> Class & {
    props_.push_back(luakit::ro_prop<M>(name));
    members_.push_back({doc::Kind::ro_field, name, {&doc::describe_prop<M>}});
    return *this;
  }

  // A computed field, read-only or read-write.
  template <auto Get>
  auto accessor(const char *name) -> Class & {
    props_.push_back(luakit::accessor<Get>(name));
    members_.push_back({doc::Kind::ro_field, name, {&doc::describe_accessor<Get>}});
    return *this;
  }

  template <auto Get, auto Set>
  auto accessor(const char *name) -> Class & {
    props_.push_back(luakit::accessor<Get, Set>(name));
    members_.push_back({doc::Kind::field, name, {&doc::describe_accessor<Get>}});
    return *this;
  }

  // A constructor, which goes in the module table rather than the metatable:
  // Lua calls it as mod.new(...), not obj:new(...).
  template <typename... A>
  auto ctor(const char *name = "new") -> Class & {
    module_fns_.push_back({name, luakit::ctor<T, A...>});
    statics_.push_back({doc::Kind::ctor, name, {&doc::describe_ctor<T, A...>}});
    return *this;
  }

  // Several constructors under one name.
  //
  //   .ctors<luakit::Args<>, luakit::Args<double, double>>("new")
  template <typename... Lists>
  auto ctors(const char *name = "new") -> Class & {
    module_fns_.push_back({name, ctor_overload<T, Lists...>});
    statics_.push_back({doc::Kind::ctor, name, {&doc::CtorOf<T, Lists>::run...}});
    return *this;
  }

  // A free function alongside the constructors in the module table.
  template <auto F>
  auto fn(const char *name) -> Class & {
    module_fns_.push_back({name, luakit::fn<F>});
    statics_.push_back({doc::Kind::fn, name, {&doc::describe_fn<F>}});
    return *this;
  }

  auto raw_fn(const char *name, core::CFunction f) -> Class & {
    module_fns_.push_back({name, f});
    statics_.push_back({doc::Kind::fn, name, {}});
    return *this;
  }

  // An enum's names, as a table in the module: `world.Facing.north`.
  //
  // Puts the valid spellings where a script can see them, rather than leaving
  // them knowable only by misspelling one and reading the error that lists
  // them. The definition generator turns the same set into a literal union.
  template <typename E>
  auto enum_(const char *name) -> Class & {
    tables_.push_back({name, &push_enum_table<E>});
    statics_.push_back({doc::Kind::enumeration, name, {&doc::describe_enum_table<E>}});
    return *this;
  }

  // Registers the class. Pushes nothing.
  auto build() -> void {
    terminate();
    Userdata<T>::register_class(L_, methods_.data(), meta_.empty() ? nullptr : meta_.data(),
                                props_.empty() ? nullptr : props_.data());
    record_doc();
  }

  // Registers the class and pushes the module table holding whatever ctor()
  // and fn() named. Returns 1, so a luaopen_ function can return it directly.
  auto build_module() -> int {
    build();
    core::aux::newlib(L_, module_fns_.data());
    for (const auto &entry : tables_) {
      entry.second(L_);
      core::setfield(L_, -2, entry.first);
    }
    return 1;
  }

 private:
  // The Reg arrays are NUL-terminated by convention, and luaL_setfuncs walks
  // until it finds that entry rather than being told a count.
  auto terminate() -> void {
    methods_.push_back({nullptr, nullptr});
    module_fns_.push_back({nullptr, nullptr});
    if (!meta_.empty()) meta_.push_back({nullptr, nullptr});
    if (!props_.empty()) props_.push_back(prop_end);
  }

  // What the class looks like from Lua, for the definition generator. Costs a
  // few string copies once per registration and nothing thereafter.
  auto record_doc() -> void {
    doc::ClassDoc entry;
    entry.name = Metatable<T>::k_name;
    entry.module = module_;
    entry.bases = base_names(static_cast<typename detail::all_bases<T>::type *>(nullptr));
    entry.members = members_;
    entry.statics = statics_;
    doc::record(std::move(entry));
  }

  template <typename... Bs>
  static auto base_names(Bases<Bs...> *) -> std::vector<std::string> {
    return {std::string(Metatable<Bs>::k_name)...};
  }

  core::State *L_;
  std::string module_;
  std::vector<core::aux::Reg> methods_;
  std::vector<core::aux::Reg> meta_;
  std::vector<core::aux::Reg> module_fns_;
  std::vector<PropertyReg> props_;
  std::vector<std::pair<const char *, void (*)(core::State *)>> tables_;
  std::vector<doc::Member> members_;
  std::vector<doc::Member> statics_;
};

}  // namespace luakit
