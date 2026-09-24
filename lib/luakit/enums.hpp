// Enums, spelled as strings on the Lua side.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/stack.hpp"

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace luakit {

// The Lua spelling of an enum. Specialize to expose one:
//
//   enum class Facing { north, south, east, west };
//
//   template <> struct luakit::EnumNames<Facing> {
//     static constexpr luakit::EnumEntry<Facing> k_values[] = {
//         {"north", Facing::north}, {"south", Facing::south},
//         {"east",  Facing::east},  {"west",  Facing::west},
//     };
//   };
//
// Scripts then say `entity:face("north")` rather than `entity:face(0)`, which
// is both readable and checkable: a misspelling is caught at the call with a
// message listing what was expected, instead of silently becoming some other
// member of the enum.
template <typename E>
struct EnumNames;

// The name is a plain const char * rather than a string_view because it ends
// up as an argument to lua_pushfstring, whose "%s" needs a NUL terminator and
// which has no length-limited conversion to use instead.
template <typename E>
struct EnumEntry {
  const char *name;
  E value;
};

template <typename E>
concept NamedEnum = std::is_enum_v<E> && requires { EnumNames<E>::k_values; };

// Pushes { north = "north", south = "south", ... }, so a script can write
// world.Facing.north and find out what the alternatives are by looking rather
// than by misspelling one and reading the error.
//
// The values are the strings themselves, because that is what an enum *is* on
// the Lua side: `c.facing = world.Facing.north` and `c.facing = "north"` are
// the same assignment, and neither is more correct than the other.
template <NamedEnum E>
auto push_enum_table(core::State *L) -> void {
  core::createtable(L, 0, static_cast<int>(std::size(EnumNames<E>::k_values)));
  for (const auto &entry : EnumNames<E>::k_values) {
    core::pushstring(L, entry.name);
    core::setfield(L, -2, entry.name);
  }
}

template <NamedEnum E>
struct Stack<E> {
  static_assert(std::size(EnumNames<E>::k_values) > 0, "luakit: EnumNames<E>::k_values must name at least one value");

  static constexpr const char *name = "string";

  // The E is a value; the string it was read from is not kept.
  static constexpr bool borrows = false;

  static auto test(core::State *L, int idx) noexcept -> bool {
    // Strictly a string: accepting a number here would let a script pass a
    // raw ordinal and skip the whole point of naming the values.
    if (core::type(L, idx) != core::TSTRING) return false;
    std::size_t n = 0;
    const char *s = core::tolstring(L, idx, &n);
    return lookup(std::string_view(s, n)) != nullptr;
  }

  static auto check(core::State *L, int idx) -> void {
    core::aux::checktype(L, idx, core::TSTRING);
    std::size_t n = 0;
    const char *s = core::tolstring(L, idx, &n);
    if (lookup(std::string_view(s, n))) return;

    // Nothing with a destructor is alive, so the longjmp out of argerror is
    // safe. The message lists the alternatives, which is most of its value.
    core::aux::argerror(L, idx, core::pushfstring(L, "'%s' is not one of %s", s, alternatives(L)));
  }

  static auto get(core::State *L, int idx) noexcept -> E {
    std::size_t n = 0;
    const char *s = core::tolstring(L, idx, &n);
    // check() has already run, so the lookup succeeds. The fallback is there
    // only because get() must not raise, and returning the first named value
    // is the least surprising thing an unreachable branch can do.
    const EnumEntry<E> *hit = lookup(std::string_view(s, n));
    return hit ? hit->value : EnumNames<E>::k_values[0].value;
  }

  static auto push(core::State *L, E v) -> int {
    for (const auto &entry : EnumNames<E>::k_values) {
      if (entry.value == v) {
        core::pushstring(L, entry.name);
        return 1;
      }
    }
    // An enum holding something the table does not name -- a cast, or a
    // combination of flags. Saying so beats pushing a plausible wrong name,
    // and a script comparing it against any valid name gets false either way.
    core::pushfstring(L, "<unnamed enum value %d>", static_cast<int>(v));
    return 1;
  }

 private:
  static auto lookup(std::string_view s) noexcept -> const EnumEntry<E> * {
    for (const auto &entry : EnumNames<E>::k_values) {
      if (std::string_view(entry.name) == s) return &entry;
    }
    return nullptr;
  }

  // Builds "'north', 'south', 'east'" as a Lua string, for the error above.
  // Concatenating on the Lua stack keeps the whole thing free of C++ objects
  // that argerror's longjmp would skip over.
  static auto alternatives(core::State *L) -> const char * {
    int pieces = 0;
    for (const auto &entry : EnumNames<E>::k_values) {
      if (pieces > 0) {
        core::pushliteral(L, ", ");
        ++pieces;
      }
      core::pushfstring(L, "'%s'", entry.name);
      ++pieces;
    }
    if (pieces == 0) return "(nothing)";
    core::concat(L, pieces);
    return core::tostring(L, -1);
  }
};

}  // namespace luakit
