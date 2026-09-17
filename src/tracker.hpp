#pragma once

#include "luakit/api.hpp"

extern "C" auto luaopen_tracker(lua::State *L) -> int;
