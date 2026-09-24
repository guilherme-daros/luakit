// The value leaves the stack before get() returns, so a view would dangle.
#include <string_view>
#include "luakit/table.hpp"
auto read(const luakit::Table &t) -> std::string_view {
  return t.get<std::string_view>("k");
}
