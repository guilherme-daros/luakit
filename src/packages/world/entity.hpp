#pragma once

#include "vec2.hpp"

#include "luakit/core/api.hpp"
#include "luakit/userdata.hpp"

#include <string>
#include <utility>

namespace world {

// The engine's own objects. Scripts see pointers to these and never own one.
class Entity {
 public:
  std::string name;
  Vec2 position;

  explicit Entity(std::string n) : name(std::move(n)) {}
  virtual ~Entity() = default;

  // Lua holds these by address, so copying one would give a script a handle to
  // something the engine does not know about.
  Entity(const Entity &) = delete;
  auto operator=(const Entity &) -> Entity & = delete;

  auto move_by(Vec2 delta) -> Entity & {
    position = position.plus(delta);
    return *this;
  }

  auto describe() const -> std::string { return "<" + name + " at " + position.describe() + ">"; }
};

auto register_entity(luakit::core::State *L) -> void;

}  // namespace world

// Cannot live inside namespace world: a specialization must name the primary
// template's namespace.
template <>
struct luakit::Metatable<world::Entity> {
  static constexpr const char *k_name = "world.Entity";
};
