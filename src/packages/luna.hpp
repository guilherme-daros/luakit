#pragma once

#include "luakit/core/api.hpp"

extern "C" auto luaopen_luna(lua::State *L) -> int;
