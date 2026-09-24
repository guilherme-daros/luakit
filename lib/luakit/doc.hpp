// What the binding knows about itself, and LuaLS definitions built from it.
//
// A mod author writing against a C++ host gets no help from their editor: the
// API exists only as template instantiations. But Class<T> and Library already
// hold every name, every C++ signature, every base class and -- through
// EnumNames -- the exact set of strings an enum field accepts. All of that is
// enough to emit `---@meta` definitions, and then `goblin.facing = "nrth"` is
// an editor error instead of a runtime one.
//
// Nothing here costs anything at registration time beyond pushing a name and a
// function pointer into a vector: the type names are rendered only when
// something actually asks to emit. So a shipping binary can carry --emit-defs
// rather than needing a separate build.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/enums.hpp"
#include "luakit/function.hpp"
#include "luakit/overload.hpp"
#include "luakit/property.hpp"
#include "luakit/stack.hpp"
#include "luakit/userdata.hpp"
#include "luakit/variadic.hpp"

#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luakit::doc {

// ------------------------------------------------------------ type names
//
// The Lua spelling of a C++ type, as LuaLS wants to read it.
//
// Most of this is already known: Stack<T>::name distinguishes "integer" from
// "number" and names a registered class by its metatable name, which is
// exactly what an annotation needs. The specializations below are the cases
// where LuaLS wants something richer than one word -- an element type, a key
// and value type, an optional, or the literal union an enum really is.

template <typename T>
struct LuaType;

template <NamedEnum E>
auto enum_union() -> std::string {
  std::string out;
  for (const auto &entry : EnumNames<E>::k_values) {
    if (!out.empty()) out += "|";
    out += '"';
    out += entry.name;
    out += '"';
  }
  return out;
}

template <typename T>
struct LuaType {
  static auto get() -> std::string {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;

    if constexpr (std::is_void_v<U>) {
      return "nil";
    } else if constexpr (!std::is_same_v<U, T>) {
      return LuaType<U>::get();  // strip cv and ref, then look again
    } else if constexpr (std::is_pointer_v<U>) {
      return LuaType<std::remove_cv_t<std::remove_pointer_t<U>>>::get();
    } else if constexpr (NamedEnum<U>) {
      // The whole point: an enum field is not a string, it is one of these
      // strings, and saying so is what makes a typo catchable in the editor.
      return enum_union<U>();
    } else {
      return std::string(Stack<detail::stack_key_t<U>>::name);
    }
  }
};

template <typename T>
struct LuaType<std::optional<T>> {
  static auto get() -> std::string { return LuaType<T>::get() + "?"; }
};

template <typename T, typename A>
struct LuaType<std::vector<T, A>> {
  static auto get() -> std::string { return LuaType<T>::get() + "[]"; }
};

template <typename T, typename C, typename A>
struct LuaType<std::set<T, C, A>> {
  static auto get() -> std::string { return LuaType<T>::get() + "[]"; }
};

template <typename T, typename H, typename E, typename A>
struct LuaType<std::unordered_set<T, H, E, A>> {
  static auto get() -> std::string { return LuaType<T>::get() + "[]"; }
};

template <typename K, typename V, typename C, typename A>
struct LuaType<std::map<K, V, C, A>> {
  static auto get() -> std::string { return "table<" + LuaType<K>::get() + ", " + LuaType<V>::get() + ">"; }
};

template <typename K, typename V, typename H, typename E, typename A>
struct LuaType<std::unordered_map<K, V, H, E, A>> {
  static auto get() -> std::string { return "table<" + LuaType<K>::get() + ", " + LuaType<V>::get() + ">"; }
};

// pair, array and tuple are fixed-length sequence tables when used as a value
// (a returned tuple is the exception, and is handled in fill_returns).
//
// A uniform one is an array and says so. A heterogeneous one has no spelling
// every LuaLS version agrees on, so it stays "table" rather than risking an
// annotation an editor would reject.
template <typename... Ts>
auto fixed_sequence_type() -> std::string {
  const std::string names[] = {LuaType<Ts>::get()...};
  for (const auto &n : names) {
    if (n != names[0]) return "table";
  }
  return names[0] + "[]";
}

template <typename A, typename B>
struct LuaType<std::pair<A, B>> {
  static auto get() -> std::string { return fixed_sequence_type<A, B>(); }
};

template <typename T, std::size_t N>
struct LuaType<std::array<T, N>> {
  static auto get() -> std::string { return LuaType<T>::get() + "[]"; }
};

template <typename... Ts>
struct LuaType<std::tuple<Ts...>> {
  static auto get() -> std::string {
    if constexpr (sizeof...(Ts) == 0) {
      return "table";
    } else {
      return fixed_sequence_type<Ts...>();
    }
  }
};

template <typename T>
struct LuaType<std::shared_ptr<T>> {
  static auto get() -> std::string { return LuaType<T>::get(); }
};

template <typename T>
struct LuaType<std::unique_ptr<T>> {
  static auto get() -> std::string { return LuaType<T>::get(); }
};

// ------------------------------------------------------------ signatures

struct Signature {
  std::vector<std::string> params;
  std::vector<std::string> returns;
  bool varargs = false;
};

// Filled on demand, so registration itself builds no strings.
using Describe = void (*)(Signature &);

template <typename... A>
auto fill_params(Signature &s) -> void {
  (
      [&] {
        // A Variadic is not a parameter with a type, it is "and the rest".
        if constexpr (std::is_same_v<detail::stack_key_t<A>, Variadic>) {
          s.varargs = true;
        } else {
          s.params.push_back(LuaType<A>::get());
        }
      }(),
      ...);
}

template <typename Tup>
struct ParamsOf;

template <typename... A>
struct ParamsOf<std::tuple<A...>> {
  static auto run(Signature &s) -> void { fill_params<A...>(s); }
};

// A returned tuple is several Lua results, matching what the binding does.
template <typename R>
auto fill_returns(Signature &s) -> void {
  using U = std::decay_t<R>;
  if constexpr (std::is_void_v<U>) {
    return;
  } else if constexpr (detail::is_tuple<U>::value) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (s.returns.push_back(LuaType<std::tuple_element_t<I, U>>::get()), ...);
    }(std::make_index_sequence<std::tuple_size_v<U>>{});
  } else {
    s.returns.push_back(LuaType<R>::get());
  }
}

template <auto F>
auto describe_fn(Signature &s) -> void {
  using sig = detail::signature<decltype(F)>;
  ParamsOf<typename sig::args>::run(s);
  fill_returns<typename sig::ret>(s);
}

template <typename T, typename... A>
auto describe_ctor(Signature &s) -> void {
  fill_params<A...>(s);
  s.returns.push_back(LuaType<T>::get());
}

template <typename T, typename Lists>
struct CtorOf;

template <typename T, typename... A>
struct CtorOf<T, Args<A...>> {
  static auto run(Signature &s) -> void { describe_ctor<T, A...>(s); }
};

// A data member exposed as a field: no parameters, one type.
template <auto M>
auto describe_prop(Signature &s) -> void {
  s.returns.push_back(LuaType<typename detail::member_object<decltype(M)>::type>::get());
}

// A computed field: the getter's return type is the field's type.
template <auto Get>
auto describe_accessor(Signature &s) -> void {
  fill_returns<typename detail::signature<decltype(Get)>::ret>(s);
}

// An enum exposed as a table of its own names. The "return" is the literal
// union, which is what the table's values are.
template <NamedEnum E>
auto describe_enum_table(Signature &s) -> void {
  s.returns.push_back(enum_union<E>());
}

// ------------------------------------------------------------- registry

enum class Kind { method, field, ro_field, ctor, fn, enumeration };

struct Member {
  Kind kind = Kind::method;
  std::string name;

  // One entry per signature. More than one means an overload set; none means a
  // raw_ binding, where there is a C function and nothing to introspect.
  std::vector<Describe> describes;
};

struct ClassDoc {
  std::string name;    // the metatable name, e.g. "world.Creature"
  std::string module;  // the module its constructors live in, if any
  std::vector<std::string> bases;
  std::vector<Member> members;  // the instance side
  std::vector<Member> statics;  // the module side: constructors, free functions
};

struct ModuleDoc {
  std::string name;
  std::vector<Member> fns;
};

struct Registry {
  std::vector<ClassDoc> classes;
  std::vector<ModuleDoc> modules;
};

inline auto registry() -> Registry & {
  static Registry r;
  return r;
}

// Records a class, replacing any earlier one of the same name.
//
// Replacing rather than appending is what makes this survive hot reload: a
// luaopen_ function runs again on every require, and each run describes the
// same class afresh.
inline auto record(ClassDoc entry) -> void {
  auto &all = registry().classes;
  for (auto &existing : all) {
    if (existing.name == entry.name) {
      existing = std::move(entry);
      return;
    }
  }
  all.push_back(std::move(entry));
}

inline auto record(ModuleDoc entry) -> void {
  auto &all = registry().modules;
  for (auto &existing : all) {
    if (existing.name == entry.name) {
      existing = std::move(entry);
      return;
    }
  }
  all.push_back(std::move(entry));
}

// --------------------------------------------------------------- output

namespace detail_emit {

inline auto render(const Signature &s, bool method) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < s.params.size(); ++i) {
    out += "---@param a" + std::to_string(i + 1) + " " + s.params[i] + "\n";
  }
  if (s.varargs) out += "---@vararg any\n";
  for (const auto &r : s.returns) out += "---@return " + r + "\n";
  (void)method;
  return out;
}

inline auto arg_list(const Signature &s) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < s.params.size(); ++i) {
    if (i > 0) out += ", ";
    out += "a" + std::to_string(i + 1);
  }
  if (s.varargs) out += out.empty() ? "..." : ", ...";
  return out;
}

// "fun(a1: number, a2: string): number", for ---@overload.
inline auto fun_type(const Signature &s) -> std::string {
  std::string out = "fun(";
  for (std::size_t i = 0; i < s.params.size(); ++i) {
    if (i > 0) out += ", ";
    out += "a" + std::to_string(i + 1) + ": " + s.params[i];
  }
  if (s.varargs) out += s.params.empty() ? "..." : ", ...";
  out += ")";

  for (std::size_t i = 0; i < s.returns.size(); ++i) {
    out += i == 0 ? ": " : ", ";
    out += s.returns[i];
  }
  return out;
}

inline auto signatures_of(const Member &m) -> std::vector<Signature> {
  std::vector<Signature> out;
  for (Describe d : m.describes) {
    Signature s;
    d(s);
    out.push_back(std::move(s));
  }
  return out;
}

// A member of whatever kind, as it appears on `holder`.
inline auto emit_member(const Member &m, const std::string &holder, const char *sep) -> std::string {
  if (m.kind == Kind::enumeration) {
    Signature s;
    if (!m.describes.empty()) m.describes[0](s);
    const std::string values = s.returns.empty() ? "string" : s.returns[0];
    return "---@type table<string, " + values + ">\n" + holder + "." + m.name + " = {}\n\n";
  }

  const auto sigs = signatures_of(m);
  if (sigs.empty()) {
    // A raw_ binding. Say it exists and takes anything, which is still better
    // than leaving the editor to report an unknown field.
    return "---@vararg any\nfunction " + holder + sep + m.name + "(...) end\n\n";
  }

  std::string out;
  for (std::size_t i = 1; i < sigs.size(); ++i) out += "---@overload " + fun_type(sigs[i]) + "\n";
  out += render(sigs[0], sep[0] == ':');
  out += "function " + holder + sep + m.name + "(" + arg_list(sigs[0]) + ") end\n\n";
  return out;
}

// The local variable a class is described through: the last dotted component
// of its metatable name, so "world.Creature" writes as `Creature`.
inline auto local_name(const std::string &metatable_name) -> std::string {
  const std::size_t dot = metatable_name.rfind('.');
  std::string out = dot == std::string::npos ? metatable_name : metatable_name.substr(dot + 1);
  for (char &c : out) {
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  }
  return out;
}

inline auto emit_class(const ClassDoc &c) -> std::string {
  std::string out = "---@class " + c.name;
  for (std::size_t i = 0; i < c.bases.size(); ++i) out += (i == 0 ? " : " : ", ") + c.bases[i];
  out += "\n";

  // Fields go in the @class block; methods follow it as functions.
  for (const auto &m : c.members) {
    if (m.kind != Kind::field && m.kind != Kind::ro_field) continue;
    Signature s;
    if (!m.describes.empty()) m.describes[0](s);
    const std::string type = s.returns.empty() ? "any" : s.returns[0];
    out += "---@field " + m.name + " " + type + (m.kind == Kind::ro_field ? "  # read-only\n" : "\n");
  }

  const std::string local = local_name(c.name);
  out += "local " + local + " = {}\n\n";

  for (const auto &m : c.members) {
    if (m.kind == Kind::field || m.kind == Kind::ro_field) continue;
    out += emit_member(m, local, ":");
  }
  return out;
}

}  // namespace detail_emit

// The LuaLS definitions for one module, ready to be written next to the mods
// that require it.
//
// Classes whose constructors live in this module are described here too, since
// that is where a mod author meets them.
inline auto emit_module(const std::string &module_name) -> std::string {
  std::string out = "---@meta " + module_name + "\n\n";
  out += "-- Generated by luakit from the C++ registration. Do not edit.\n\n";

  for (const auto &c : registry().classes) {
    if (c.module == module_name) out += detail_emit::emit_class(c);
  }

  const std::string local = detail_emit::local_name(module_name);
  out += "---@class " + module_name + "_module\n";
  out += "local " + local + " = {}\n\n";

  for (const auto &c : registry().classes) {
    if (c.module != module_name) continue;
    for (const auto &m : c.statics) out += detail_emit::emit_member(m, local, ".");
  }

  for (const auto &mod : registry().modules) {
    if (mod.name != module_name) continue;
    for (const auto &m : mod.fns) out += detail_emit::emit_member(m, local, ".");
  }

  out += "return " + local + "\n";
  return out;
}

// Every module that registered anything, in registration order.
inline auto module_names() -> std::vector<std::string> {
  std::vector<std::string> out;
  auto add = [&](const std::string &name) {
    if (name.empty()) return;
    for (const auto &seen : out) {
      if (seen == name) return;
    }
    out.push_back(name);
  };
  for (const auto &c : registry().classes) add(c.module);
  for (const auto &m : registry().modules) add(m.name);
  return out;
}

}  // namespace luakit::doc
