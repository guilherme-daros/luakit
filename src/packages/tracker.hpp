#pragma once

#include "luakit/core/api.hpp"

extern "C" auto luaopen_tracker(luakit::core::State *L) -> int;
