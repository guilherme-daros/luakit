#pragma once

#include "luakit/interpreter.hpp"

// The tour of the binding: sets up the packages and lua/init.lua, then does
// what an engine does -- runs frames, drives a scripted task, destroys
// something a mod still has a handle to, and reloads a plugin. main.cpp is
// just this call plus the top-level error handling.
namespace demo {

auto run(luakit::Interpreter &interpreter) -> void;

}  // namespace demo
