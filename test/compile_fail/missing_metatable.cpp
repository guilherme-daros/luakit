// A class with no Metatable<T> is the most common mistake, and gets its own
// message rather than "no Stack<T> for this type".
#include "luakit/function.hpp"
struct Unregistered {};
auto bad(Unregistered) -> void {
}
auto binding = luakit::fn<bad>;
