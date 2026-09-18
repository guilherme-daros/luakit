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

// Rendered only on failure, so a passing check costs no formatting.
inline auto text(const std::string &s) -> std::string {
  return s;
}
inline auto text(const char *s) -> std::string {
  return s ? s : "(null)";
}
template <typename T>
auto text(const T &v) -> std::string {
  return std::to_string(v);
}

// The comparison helpers exist so the macros can name each operand once.
// Expanding an operand twice would run it twice, and a check whose operand
// drives a coroutine or mutates state is then testing something other than
// what it reads like.
template <typename A, typename B>
auto report_eq(const A &a, const B &b, const char *what) -> void {
  const bool ok = (a == b);
  report(ok, what, ok ? std::string() : "got " + text(a) + ", want " + text(b));
}

template <typename A, typename B>
auto report_str(const A &a, const B &b, const char *what) -> void {
  const std::string lhs(a);
  const std::string rhs(b);
  const bool ok = (lhs == rhs);
  report(ok, what, ok ? std::string() : "got '" + lhs + "', want '" + rhs + "'");
}

#define CHECK(cond) ::t::report((cond), #cond, "")
#define CHECK_EQ(a, b) ::t::report_eq((a), (b), #a " == " #b)
#define CHECK_STR(a, b) ::t::report_str((a), (b), #a " == " #b)

inline auto section(const char *name) -> void {
  std::printf("%s\n", name);
}

inline auto summary() -> int {
  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace t
