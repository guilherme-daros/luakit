#include "vec2.hpp"

#include "luakit/class.hpp"

namespace world {

auto register_vec2(luakit::core::State *L) -> void {
  luakit::Class<Vec2>(L, "world")
      .method<&Vec2::length>("length")
      .overload<static_cast<Vec2 (Vec2::*)(Vec2) const>(&Vec2::plus),
                static_cast<Vec2 (Vec2::*)(double) const>(&Vec2::plus)>("plus")
      .prop<&Vec2::x>("x")
      .prop<&Vec2::y>("y")
      .meta<&Vec2::describe>("__tostring")
      .build();
}

}  // namespace world
