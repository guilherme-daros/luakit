#include "tracker.hpp"

#include "luakit/class.hpp"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace core = luakit::core;

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

auto luaopen_tracker(core::State *L) -> int {
  using namespace tracker;

  return luakit::Class<Tracker>(L, "tracker")
      .method<&Tracker::add>("add")
      .method<&Tracker::mean>("mean")
      .method<&Tracker::max>("max")
      .method<&Tracker::name>("name")
      .meta<&Tracker::count>("__len")
      .meta<&Tracker::describe>("__tostring")
      .ctor<std::string>("new")
      .build_module();
}
