// Lua aligns userdata for the fundamental types and no further.
#include "luakit/userdata.hpp"
struct alignas(64) Wide {
  double v[8];
};
template <>
struct luakit::Metatable<Wide> {
  static constexpr const char *k_name = "cf.Wide";
};
auto sized = sizeof(luakit::Userdata<Wide>);
