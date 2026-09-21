#pragma once

#include "entity.hpp"

#include "luakit/core/api.hpp"
#include "luakit/enums.hpp"
#include "luakit/userdata.hpp"

#include <algorithm>

namespace world {

enum class Facing { north, south, east, west };

// Single inheritance here because that is what a world like this needs.
// Several bases work the same way, with the pointer adjusted for each.
class Creature : public Entity {
 public:
  using Entity::Entity;

  auto damage(int amount) -> Creature & {
    hp_ = std::max(0, hp_ - amount);
    return *this;
  }

  auto health() const -> int { return hp_; }

  // A setter is what makes this worth being an accessor rather than a plain
  // field: the clamp applies however a script writes to it.
  auto set_health(int v) -> void { hp_ = std::clamp(v, 0, 100); }

  auto alive() const -> bool { return hp_ > 0; }

  auto facing() const -> Facing { return facing_; }
  auto set_facing(Facing f) -> void { facing_ = f; }

 private:
  int hp_ = 100;
  Facing facing_ = Facing::north;
};

auto register_creature(luakit::core::State *L) -> void;

}  // namespace world

// Cannot live inside namespace world: a specialization must name the primary
// template's namespace.
template <>
struct luakit::Metatable<world::Creature> {
  static constexpr const char *k_name = "world.Creature";
  using bases = luakit::Bases<world::Entity>;
};

// An enum reaches Lua as a string, so a script writes `c.facing = "north"` and
// a misspelling is caught at the assignment with the alternatives listed.
template <>
struct luakit::EnumNames<world::Facing> {
  static constexpr luakit::EnumEntry<world::Facing> k_values[] = {
      {"north", world::Facing::north},
      {"south", world::Facing::south},
      {"east",  world::Facing::east },
      {"west",  world::Facing::west },
  };
};
