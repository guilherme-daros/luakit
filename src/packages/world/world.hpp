#pragma once

#include "luakit/core/api.hpp"
#include "luakit/module.hpp"

auto luaopen_world(luakit::core::State *L) -> int;

namespace mod {

inline auto world() -> luakit::Module {
  return {"world", luaopen_world};
}

}  // namespace mod

// The host side of the package. main.cpp drives these the way a game loop
// would: the script says what should happen, the engine decides when.
namespace world {

// Fires every handler a script registered with on_tick. A handler that
// faults is reported and skipped, so one bad plugin does not stop the frame.
auto tick(double dt) -> void;

auto handler_count() -> int;

// How many times luaopen_world has run, for the hot-reload demonstration.
auto open_count() -> int;

// Destroys a creature and severs any handle a script still holds.
auto destroy(const char *name) -> bool;

// Drops the engine's own reference to a summoned creature, leaving whatever
// Lua still holds as the only thing keeping it alive.
auto release_summoned() -> void;

auto population() -> int;

// Releases everything the package holds that points into the interpreter.
//
// This has to run before the Interpreter is destroyed. A luakit::Function owns
// a registry reference and gives it back in its destructor, so one left alive
// past lua_close would unref against a freed lua_State. Storing handlers in a
// namespace-scope container makes that easy to get wrong, because static
// destruction happens after main returns -- which is exactly the shape a real
// host's plugin manager tends to have.
auto shutdown() -> void;

}  // namespace world
