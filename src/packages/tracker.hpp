#pragma once

#include "luakit/core/api.hpp"

extern "C" auto luaopen_tracker(lua::State *L) -> int;
