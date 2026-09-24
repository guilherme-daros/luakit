// A Variadic reads to the top of the stack, so anything after it starves.
#include "luakit/function.hpp"
#include "luakit/variadic.hpp"
auto bad(luakit::Variadic, int) -> void {
}
auto binding = luakit::fn<bad>;
