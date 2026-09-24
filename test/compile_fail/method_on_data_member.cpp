// prop<> is the one for a data member; method<> should say so.
#include "luakit/class.hpp"
struct Thing {
  int field = 0;
};
template <>
struct luakit::Metatable<Thing> {
  static constexpr const char *k_name = "cf.Thing";
};
auto binding = luakit::method<&Thing::field>;
