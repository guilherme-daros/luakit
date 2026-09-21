#pragma once

#include "luakit/core/api.hpp"
#include "luakit/error.hpp"
#include "luakit/module.hpp"
#include "luakit/script.hpp"
#include "luakit/stack.hpp"
#include "luakit/table.hpp"
#include "luakit/userdata.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace luakit {

// The standard libraries, for opening one at a time with Interpreter::openlib
// rather than all of them with Interpreter::openlibs. Named and ordered the
// way luaL_openlibs' own internal table is.
enum class Lib { base, package, coroutine, table, io, os, string, math, utf8, debug };

// Owns a lua_State. This is the host-side entry point; code running inside a
// bound callback is handed a borrowed core::State* instead and must not own it,
// which is why Stack<T> and the fn/method wrappers all take the raw pointer.
class Interpreter {
 public:
  Interpreter() : L_(core::aux::newstate()) {
    if (!L_) throw Error("luakit: cannot create lua_State");
  }

  ~Interpreter() {
    if (L_) core::close(L_);
  }

  Interpreter(Interpreter &&other) noexcept : L_(std::exchange(other.L_, nullptr)) {}
  auto operator=(Interpreter &&other) noexcept -> Interpreter & {
    if (this != &other) {
      if (L_) core::close(L_);
      L_ = std::exchange(other.L_, nullptr);
    }
    return *this;
  }

  Interpreter(const Interpreter &) = delete;
  auto operator=(const Interpreter &) -> Interpreter & = delete;

  // Escape hatch, for mixing with hand-written C API code.
  auto raw() const noexcept -> core::State * { return L_; }

  auto openlibs() -> Interpreter & {
    core::aux::openlibs(L_);
    return *this;
  }

  // Opens a single standard library, the way luaL_openlibs would open just
  // that one: registered in package.loaded and as a global under its usual
  // name (e.g. Lib::string gives you both package.loaded.string and _G.string).
  auto openlib(Lib lib) -> Interpreter & {
    switch (lib) {
      case Lib::base:
        core::aux::requiref(L_, core::GNAME, core::lib::base, 1);
        break;
      case Lib::package:
        core::aux::requiref(L_, core::LOADLIBNAME, core::lib::package, 1);
        break;
      case Lib::coroutine:
        core::aux::requiref(L_, core::COLIBNAME, core::lib::coroutine, 1);
        break;
      case Lib::table:
        core::aux::requiref(L_, core::TABLIBNAME, core::lib::table, 1);
        break;
      case Lib::io:
        core::aux::requiref(L_, core::IOLIBNAME, core::lib::io, 1);
        break;
      case Lib::os:
        core::aux::requiref(L_, core::OSLIBNAME, core::lib::os, 1);
        break;
      case Lib::string:
        core::aux::requiref(L_, core::STRLIBNAME, core::lib::string, 1);
        break;
      case Lib::math:
        core::aux::requiref(L_, core::MATHLIBNAME, core::lib::math, 1);
        break;
      case Lib::utf8:
        core::aux::requiref(L_, core::UTF8LIBNAME, core::lib::utf8, 1);
        break;
      case Lib::debug:
        core::aux::requiref(L_, core::DBLIBNAME, core::lib::debug, 1);
        break;
    }
    core::pop(L_, 1);  // requiref leaves the module table on the stack
    return *this;
  }

  // Advertise a module to require without loading it. Must follow openlibs
  // (or at least openlib(Lib::package)), which is what creates `package`.
  auto preload(Module lib) -> Interpreter & {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::PRELOAD_TABLE);
    core::pushcfunction(L_, lib.open);
    core::setfield(L_, -2, lib.name);
    core::pop(L_, 1);
    return *this;
  }

  template <typename T>
  auto bind(const core::aux::Reg *methods, const core::aux::Reg *meta = nullptr, const PropertyReg *props = nullptr)
      -> Interpreter & {
    Userdata<T>::register_class(L_, methods, meta, props);
    return *this;
  }

  auto script(std::string_view code, const char *chunkname = "=chunk") -> void {
    run(core::aux::loadbuffer(L_, code.data(), code.size(), chunkname), "cannot load");
  }

  // Accepts precompiled bytecode or source; luaL_loadbuffer sniffs the
  // "\x1bLua" signature. The chunk name is only a label for tracebacks.
  auto script_bytecode(const void *data, std::size_t len, const char *chunkname) -> void {
    run(core::aux::loadbuffer(L_, static_cast<const char *>(data), len, chunkname), "cannot load");
  }

  auto script_file(const char *path) -> void { run(core::aux::loadfile(L_, path), "cannot load"); }

  // Runs a Script from luakit_embed_dir's generated table, whichever of
  // embedded bytes or a disk path it turned out to hold.
  auto load(const Script &s) -> void {
    if (s.data) {
      script_bytecode(s.data, s.len, s.chunkname);
    } else {
      script_file(s.path);
    }
  }

  // Forgets a loaded module, so the next require runs its opener or file
  // again. Unknown module names are not an error: the point is to end up with
  // it not loaded.
  auto unload(const char *module) -> Interpreter & {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::LOADED_TABLE);
    core::pushnil(L_);
    core::setfield(L_, -2, module);
    core::pop(L_, 1);
    return *this;
  }

  // Reloads a module in place, which is what makes editing a plugin without
  // restarting the host possible.
  //
  // What this does not do is retrofit the new code onto anything the old
  // version left behind. A table a script already captured, a callback the
  // host is still holding, an object built from the previous metatable: all of
  // them keep the old behaviour. Reloading works for plugins whose state lives
  // in the module table and nowhere else, which is worth designing for if hot
  // reload matters.
  //
  // A module that fails to load is left unloaded rather than half-loaded, so
  // fixing the script and reloading again is the way out.
  auto reload(const char *module) -> Interpreter & {
    unload(module);

    core::getglobal(L_, "require");
    if (!core::isfunction(L_, -1)) {
      core::pop(L_, 1);
      throw Error("luakit: reload needs the standard libraries; call openlibs first");
    }
    core::pushstring(L_, module);
    fail_if(detail::call_traced(L_, 1, 0), "error reloading");
    return *this;
  }

  // Whether a module is currently loaded.
  auto loaded(const char *module) -> bool {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::LOADED_TABLE);
    const bool present = core::getfield(L_, -1, module) != core::TNIL;
    core::pop(L_, 2);
    return present;
  }

  // The globals table, _G.
  auto globals() -> Table { return Table::globals(L_); }

  // Reads a global, throwing if it is absent or not a T. The common case by
  // far, so it is spelled out rather than left to globals().get<T>().
  template <typename T>
  auto global(const char *name) -> T {
    return globals().template get<T>(name);
  }

  // Reads a global, falling back when it is absent or the wrong type.
  template <typename T>
  auto global_or(const char *name, T fallback) -> T {
    return globals().template get_or<T>(name, std::move(fallback));
  }

  template <typename T>
  auto set_global(const char *name, T &&value) -> Interpreter & {
    globals().set(name, std::forward<T>(value));
    return *this;
  }

 private:
  // Loads then calls, converting either failure into an Error. The call is
  // traced: a script author whose plugin fails ten frames deep gets the frames
  // rather than just the innermost line.
  auto run(int load_status, const char *what) -> void {
    fail_if(load_status, what);
    fail_if(detail::call_traced(L_, 0, 0), "error running");
  }

  auto fail_if(int status, const char *what) -> void {
    if (status == core::OK) return;
    throw Error(std::string(what) + ": " + detail::take_error(L_));
  }

  core::State *L_ = nullptr;
};

}  // namespace luakit
