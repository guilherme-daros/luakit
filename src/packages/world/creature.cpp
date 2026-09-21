#include "creature.hpp"

#include "luakit/class.hpp"

namespace world {

auto register_creature(luakit::core::State *L) -> void {
  luakit::Class<Creature>(L)
      .method<&Creature::damage>("damage")
      .accessor<&Creature::health, &Creature::set_health>("hp")
      .accessor<&Creature::facing, &Creature::set_facing>("facing")
      .accessor<&Creature::alive>("alive")
      .build();
}

}  // namespace world
