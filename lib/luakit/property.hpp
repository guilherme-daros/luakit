// Fields on a registered class, so scripts write entity.hp rather than
// entity:set_hp().

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/function.hpp"
#include "luakit/guard.hpp"
#include "luakit/stack.hpp"
#include "luakit/userdata.hpp"

#include <type_traits>

namespace luakit {

namespace detail {

// Splits a pointer-to-data-member into the class and the field type.
template <typename>
struct member_object;

template <typename V, typename C>
struct member_object<V C::*> {
  using cls = C;
  using type = V;
};

template <auto M>
auto prop_get_impl(core::State *L) -> int {
  using info = member_object<decltype(M)>;
  typename info::cls *self = Userdata<typename info::cls>::check(L, 1);
  return Stack<stack_key_t<typename info::type>>::push(L, self->*M);
}

template <auto M>
auto prop_set_impl(core::State *L) -> int {
  using info = member_object<decltype(M)>;
  using value_type = stack_key_t<typename info::type>;

  typename info::cls *self = Userdata<typename info::cls>::check(L, 1);

  // Same two phases as a bound function: validate before anything is built, so
  // a rejected assignment cannot leave a half-converted value behind.
  Stack<value_type>::check(L, 2);
  self->*M = Stack<value_type>::get(L, 2);
  return 0;
}

}  // namespace detail

// A member variable, readable and writable from Lua:
//
//   luakit::Class<Entity>(L)
//       .prop<&Entity::hp>("hp")
//       .ro_prop<&Entity::name>("name")
//       .accessor<&Entity::speed, &Entity::set_speed>("speed")
//       .build();
//
// Reads and writes go through Stack<T>, so a field is checked exactly as a
// function argument would be: `entity.hp = "lots"` is refused, by name.
template <auto M>
constexpr auto prop(const char *name) -> PropertyReg {
  static_assert(std::is_member_object_pointer_v<decltype(M)>,
                "luakit: prop<> takes a pointer to a data member. For a computed field backed by "
                "getter and setter methods, use accessor<> instead.");
  return {name, &guard<&detail::prop_get_impl<M>>, &guard<&detail::prop_set_impl<M>>};
}

// A member variable Lua may read but not assign to.
template <auto M>
constexpr auto ro_prop(const char *name) -> PropertyReg {
  static_assert(std::is_member_object_pointer_v<decltype(M)>,
                "luakit: ro_prop<> takes a pointer to a data member. For a computed field backed "
                "by a getter method, use accessor<> instead.");
  return {name, &guard<&detail::prop_get_impl<M>>, nullptr};
}

// A computed field, backed by a getter method. Read-only.
//
// No new machinery: a getter is exactly what luakit::method already builds --
// a function taking self and returning a value -- so this only names it.
template <auto Get>
constexpr auto accessor(const char *name) -> PropertyReg {
  return {name, method<Get>, nullptr};
}

// A computed field with a setter as well. The setter is a method taking one
// argument, which is the same shape luakit::method binds for a call.
template <auto Get, auto Set>
constexpr auto accessor(const char *name) -> PropertyReg {
  return {name, method<Get>, method<Set>};
}

// Terminates a PropertyReg array, the way {nullptr, nullptr} terminates a Reg.
inline constexpr PropertyReg prop_end{nullptr, nullptr, nullptr};

}  // namespace luakit
