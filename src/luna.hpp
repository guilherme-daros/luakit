#pragma once

#include "luakit/api.hpp"

extern "C" auto luaopen_luna(lua::State *L) -> int;
