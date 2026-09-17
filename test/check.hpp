#pragma once

#include <cstdio>
#include <string>

namespace t {

inline int failures = 0;
inline int checks = 0;

inline auto report(bool ok, const char *what, const std::string &detail) -> void {
  ++checks;
  if (ok) return;
  ++failures;
  std::printf("  FAIL  %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
}

#define CHECK(cond) ::t::report((cond), #cond, "")
#define CHECK_EQ(a, b) \
  ::t::report((a) == (b), #a " == " #b, std::string("got ") + std::to_string(a) + ", want " + std::to_string(b))
#define CHECK_STR(a, b) \
  ::t::report(std::string(a) == std::string(b), #a " == " #b, std::string("got '") + std::string(a) + "'")

inline auto section(const char *name) -> void {
  std::printf("%s\n", name);
}

inline auto summary() -> int {
  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace t
