// The demo host's entry point: construct the interpreter, run the tour
// (demo.cpp), report a fatal error if one escapes. What the tour actually
// does is demo.cpp's job, not this file's.
//
// `--emit-defs [dir]` is the other mode: register the packages, write the
// LuaLS definitions they describe, and exit. Deliberately the same binary
// rather than a separate tool -- the definitions come out of the registration
// itself, so they can only ever be as current as the code that just ran.

#include "demo.hpp"

#include "luakit/doc.hpp"
#include "luakit/interpreter.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

auto emit_definitions(const std::string &dir) -> int {
  auto interpreter = luakit::Interpreter();
  interpreter.openlibs();

  // Opening the packages is what fills the registry: nothing is described
  // ahead of time, so what comes out is exactly what a script would see.
  demo::open_packages(interpreter);

  for (const auto &name : luakit::doc::module_names()) {
    const std::string path = dir + "/" + name + ".lua";

    std::FILE *out = std::fopen(path.c_str(), "w");
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      return 1;
    }

    const std::string text = luakit::doc::emit_module(name);
    std::fwrite(text.data(), 1, text.size(), out);
    std::fclose(out);
    std::printf("wrote %s\n", path.c_str());
  }
  return 0;
}

}  // namespace

auto main(int argc, char **argv) -> int try {
  if (argc > 1 && std::strcmp(argv[1], "--emit-defs") == 0) {
    return emit_definitions(argc > 2 ? argv[2] : ".");
  }

  auto interpreter = luakit::Interpreter();
  interpreter.openlibs();
  demo::run(interpreter);

  return 0;

} catch (const std::exception &e) {
  std::fprintf(stderr, "fatal: %s\n", e.what());
  return 1;
}
