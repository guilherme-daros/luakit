#pragma once

#include "luakit/core/api.hpp"

extern "C" auto luaopen_luna(luakit::core::State *L) -> int;
