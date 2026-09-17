#include "tracker.hpp"

#include "luakit/function.hpp"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace tracker {

class Tracker {
 public:
  explicit Tracker(std::string name) : name_(std::move(name)) {}

  ~Tracker() { std::printf("[C++] [Dtor] Tracker(\"%s\")\n", name_.c_str()); }

  // Lua holds this by address.
  Tracker(const Tracker &) = delete;
  auto operator=(const Tracker &) -> Tracker & = delete;

  // Returning *this is how a bound method chains: luakit::method sees a C&
  // return and hands back the receiver already on the stack.
  auto add(double v) -> Tracker & {
    samples_.push_back(v);
    return *this;
  }

  auto count() const noexcept -> std::size_t { return samples_.size(); }
  auto name() const noexcept -> const std::string & { return name_; }

  auto mean() const -> double {
    if (samples_.empty()) throw std::runtime_error("mean() of an empty tracker");
    return std::accumulate(samples_.begin(), samples_.end(), 0.0) / static_cast<double>(samples_.size());
  }

  auto max() const -> double {
    if (samples_.empty()) throw std::runtime_error("max() of an empty tracker");
    return *std::max_element(samples_.begin(), samples_.end());
  }

  auto describe() const -> std::string {
    return "[Tracker](\"" + name_ + "\", " + std::to_string(samples_.size()) + " samples)";
  }

 private:
  std::string name_;
  std::vector<double> samples_;
};

}  // namespace tracker

// Cannot live inside namespace tracker: a specialization must name the
// primary template's namespace.
template <>
struct luakit::Metatable<tracker::Tracker> {
  static constexpr const char *k_name = "luna.Tracker";
};

namespace tracker {

const lua::aux::Reg methods[] = {
    {"add",   luakit::method<&Tracker::add> },
    {"mean",  luakit::method<&Tracker::mean>},
    {"max",   luakit::method<&Tracker::max> },
    {"name",  luakit::method<&Tracker::name>},
    {nullptr, nullptr                       },
};

const lua::aux::Reg meta[] = {
    {"__len",      luakit::method<&Tracker::count>   },
    {"__tostring", luakit::method<&Tracker::describe>},
    {nullptr,      nullptr                           },
};

const lua::aux::Reg funcs[] = {
    {"new", luakit::ctor<Tracker, std::string>},
    {nullptr, nullptr},
};

}  // namespace tracker

extern "C" auto luaopen_tracker(lua::State *L) -> int {
  luakit::Userdata<tracker::Tracker>::register_class(L, tracker::methods, tracker::meta);

  lua::aux::newlib(L, tracker::funcs);
  return 1;
}
