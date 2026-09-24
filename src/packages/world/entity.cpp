#include "entity.hpp"

#include "luakit/class.hpp"

namespace world {

auto register_entity(luakit::core::State *L) -> void {
  luakit::Class<Entity>(L, "world")
      .method<&Entity::move_by>("move_by")
      .ro_prop<&Entity::name>("name")
      .prop<&Entity::position>("position")
      .meta<&Entity::describe>("__tostring")
      .build();
}

}  // namespace world
