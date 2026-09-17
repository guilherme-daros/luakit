#pragma once

#include "luakit/api.hpp"
#include "luakit/binding.hpp"
#include "luakit/stack.hpp"

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
// bound callback is handed a borrowed lua::State* instead and must not own it,
// which is why Stack<T> and the fn/method wrappers all take the raw pointer.
class State {
 public:
  State() : L_(lua::aux::newstate()) {
    if (!L_) throw Error("luakit: cannot create lua_State");
  }

  ~State() {
    if (L_) lua::close(L_);
  }

  State(State &&other) noexcept : L_(std::exchange(other.L_, nullptr)) {}
  auto operator=(State &&other) noexcept -> State & {
    if (this != &other) {
      if (L_) lua::close(L_);
      L_ = std::exchange(other.L_, nullptr);
    }
    return *this;
  }

  State(const State &) = delete;
  auto operator=(const State &) -> State & = delete;

  // Escape hatch, for mixing with hand-written C API code.
  auto raw() const noexcept -> lua::State * { return L_; }

  auto open_libs() -> State & {
    lua::aux::openlibs(L_);
    return *this;
  }

  // Advertise a module to require without loading it. Must follow open_libs,
  // which is what creates `package`.
  auto preload(const char *name, lua::CFunction openf) -> State & {
    lua::aux::getsubtable(L_, lua::REGISTRYINDEX, lua::PRELOAD_TABLE);
    lua::pushcfunction(L_, openf);
    lua::setfield(L_, -2, name);
    lua::pop(L_, 1);
    return *this;
  }

  template <typename T>
  auto bind(const lua::aux::Reg *methods, const lua::aux::Reg *meta = nullptr) -> State & {
    Binding<T>::register_class(L_, methods, meta);
    return *this;
  }

  auto script(std::string_view code, const char *chunkname = "=chunk") -> void {
    run(lua::aux::loadbuffer(L_, code.data(), code.size(), chunkname), "cannot load");
  }

  // Accepts precompiled bytecode or source; luaL_loadbuffer sniffs the
  // "\x1bLua" signature. The chunk name is only a label for tracebacks.
  auto script_bytecode(const void *data, std::size_t len, const char *chunkname) -> void {
    run(lua::aux::loadbuffer(L_, static_cast<const char *>(data), len, chunkname), "cannot load");
  }

  auto script_file(const char *path) -> void { run(lua::aux::loadfile(L_, path), "cannot load"); }

  template <typename T>
  auto push(T &&v) -> State & {
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
    fail_if(lua::pcall(L_, 0, 0, 0), "error running");
  }

  auto fail_if(int status, const char *what) -> void {
    if (status == lua::OK) return;
    const char *err = lua::gettop(L_) > 0 ? lua::tostring(L_, -1) : nullptr;
    std::string msg = err ? err : "?";
    if (lua::gettop(L_) > 0) lua::pop(L_, 1);
    throw Error(std::string(what) + ": " + msg);
  }

  lua::State *L_ = nullptr;
};

}  // namespace luakit
