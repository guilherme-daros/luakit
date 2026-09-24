// The yielded value is gone before resume returns.
#include <string_view>
#include "luakit/coroutine.hpp"
auto drive(luakit::Coroutine &co) -> void {
  (void)co.resume<std::string_view>();
}
