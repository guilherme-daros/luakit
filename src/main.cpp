// The demo host's entry point: construct the interpreter, run the tour
// (demo.cpp), report a fatal error if one escapes. What the tour actually
// does is demo.cpp's job, not this file's.

#include "demo.hpp"

#include "luakit/interpreter.hpp"

#include <cstdio>

auto main(int argc, char **argv) -> int try {
  (void)argc;
  (void)argv;

  auto interpreter = luakit::Interpreter();
  demo::run(interpreter);

  return 0;

} catch (const std::exception &e) {
  std::fprintf(stderr, "fatal: %s\n", e.what());
  return 1;
}
