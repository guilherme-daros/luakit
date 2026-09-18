#pragma once

#include "luakit/core/api.hpp"
#include "luakit/module.hpp"
#include "luakit/stack.hpp"
#include "luakit/userdata.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace luakit {

// Thrown for failures on the host side: loading a chunk, running it, or
// creating the interpreter. Errors raised *inside* Lua arrive as the message
// of one of these after a failed pcall.
class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

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

  auto open_libs() -> Interpreter & {
    core::aux::openlibs(L_);
    return *this;
  }

  // Advertise a module to require without loading it. Must follow open_libs,
  // which is what creates `package`.
  auto preload(Module lib) -> Interpreter & {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::PRELOAD_TABLE);
    core::pushcfunction(L_, lib.open);
    core::setfield(L_, -2, lib.name);
    core::pop(L_, 1);
    return *this;
  }

  template <typename T>
  auto bind(const core::aux::Reg *methods, const core::aux::Reg *meta = nullptr) -> Interpreter & {
    Userdata<T>::register_class(L_, methods, meta);
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

  template <typename T>
  auto push(T &&v) -> Interpreter & {
    luakit::push(L_, std::forward<T>(v));
    return *this;
  }

  template <typename T>
  auto get(int idx) -> T {
    return luakit::get<T>(L_, idx);
  }

 private:
  // Loads then calls, converting either failure into an Error.
  auto run(int load_status, const char *what) -> void {
    fail_if(load_status, what);
    fail_if(core::pcall(L_, 0, 0, 0), "error running");
  }

  auto fail_if(int status, const char *what) -> void {
    if (status == core::OK) return;
    const char *err = core::gettop(L_) > 0 ? core::tostring(L_, -1) : nullptr;
    std::string msg = err ? err : "?";
    if (core::gettop(L_) > 0) core::pop(L_, 1);
    throw Error(std::string(what) + ": " + msg);
  }

  core::State *L_ = nullptr;
};

}  // namespace luakit
