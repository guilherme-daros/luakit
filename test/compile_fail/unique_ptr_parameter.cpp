// Taking a unique_ptr out of Lua would strip an object scripts may still hold.
#include <memory>
#include "luakit/class.hpp"
struct Thing {};
template <>
struct luakit::Metatable<Thing> {
  static constexpr const char *k_name = "cf.Thing";
};
auto bad(std::unique_ptr<Thing>) -> void {
}
auto binding = luakit::fn<bad>;
