#pragma once

#include "luakit/core/api.hpp"
#include "luakit/userdata.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace world {

// A value type. Lua gets its own copy, which the collector owns, because a
// vector with no home in the engine has nothing to borrow from.
struct Vec2 {
  double x = 0;
  double y = 0;

  Vec2() = default;
  Vec2(double a, double b) : x(a), y(b) {}

  auto length() const -> double { return std::sqrt(x * x + y * y); }
  auto plus(Vec2 other) const -> Vec2 { return {x + other.x, y + other.y}; }
  auto plus(double scalar) const -> Vec2 { return {x + scalar, y + scalar}; }
  auto describe() const -> std::string {
    char buf[48];
    std::snprintf(buf, sizeof buf, "(%.1f, %.1f)", x, y);
    return buf;
  }
};

auto register_vec2(luakit::core::State *L) -> void;

}  // namespace world

// Cannot live inside namespace world: a specialization must name the primary
// template's namespace.
template <>
struct luakit::Metatable<world::Vec2> {
  static constexpr const char *k_name = "world.Vec2";
};
