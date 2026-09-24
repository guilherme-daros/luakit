// The host-side error type, and the protected call that gives it a traceback.
//
// Its own header for the same reason module.hpp is: a Function or a Coroutine
// needs to throw Error, and making them include interpreter.hpp for it would
// drag the whole facade along behind a single class.

#pragma once

#include "luakit/core/api.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace luakit {

// Thrown for failures on the host side: loading a chunk, running it, or
// creating the interpreter. Errors raised *inside* Lua arrive as the message
// of one of these after a failed pcall.
//
// what() carries the traceback appended, which is what a fatal handler wants
// to print. traceback() carries it on its own, which is what a host that logs
// the frames somewhere separate from the one-line message wants. It is empty
// for a failure that never reached Lua, such as a chunk that would not load.
class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;

  Error(const std::string &message, std::string frames)
      : std::runtime_error(frames.empty() ? message : message + "\n" + frames), traceback_(std::move(frames)) {}

  auto traceback() const noexcept -> const std::string & { return traceback_; }

 private:
  std::string traceback_;
};

namespace detail {

// The message handler for call_traced. It is worth the trouble because it runs
// at the point of the error, while the failing frames are still live -- by the
// time pcall has returned they are gone and no traceback can be recovered.
inline auto traceback_handler(core::State *L) -> int {
  const char *msg = core::tostring(L, 1);
  if (!msg) {
    // A non-string error object: a table thrown by error{code = 2}, say. If it
    // renders itself, let it; otherwise name the type rather than dropping it.
    if (core::aux::callmeta(L, 1, "__tostring") && core::type(L, -1) == core::TSTRING) return 1;
    msg = core::pushfstring(L, "(error object is a %s value)", core::aux::typename_(L, 1));
  }
  core::aux::traceback(L, L, msg, 1);
  return 1;
}

// lua_pcall with that handler installed. The handler has to sit *below* the
// function and its arguments, so it is pushed last and then moved down, and
// removed again afterwards to leave the stack as a plain pcall would.
inline auto call_traced(core::State *L, int nargs, int nresults) -> int {
  const int base = core::gettop(L) - nargs;  // where the function sits
  core::pushcfunction(L, traceback_handler);
  core::insert(L, base);
  const int status = core::pcall(L, nargs, nresults, base);
  core::remove(L, base);
  return status;
}

// Puts the stack back where it was on the way out, however that happens.
//
// Lua itself tolerates a stack left grown, but a host that calls into a plugin
// every frame does not: the error path is exactly the one that repeats, and a
// few slots leaked per call adds up to a stack overflow eventually.
//
// lua_settop is documented as able to raise in 5.4, because shrinking past a
// to-be-closed slot runs its __close. Nothing here ever calls lua_toclose, so
// there are none to run.
class StackRestore {
 public:
  StackRestore(core::State *L, int top) noexcept : L_(L), top_(top) {}
  ~StackRestore() { core::settop(L_, top_); }

  StackRestore(const StackRestore &) = delete;
  auto operator=(const StackRestore &) -> StackRestore & = delete;

 private:
  core::State *L_;
  int top_;
};

// Pops the error value a failed load or call left on top and renders it.
inline auto take_error(core::State *L) -> std::string {
  if (core::gettop(L) == 0) return "?";
  const char *err = core::tostring(L, -1);
  std::string msg = err ? err : "?";
  core::pop(L, 1);
  return msg;
}

// Splits what traceback_handler produced back into the two halves it joined.
//
// The handler calls luaL_traceback, which writes "<message>\nstack
// traceback:\n...". Rather than have the handler build something structured --
// which would mean allocating a table on an error path, at the one moment the
// state is least healthy -- the seam is simply found again here.
inline auto split_traceback(std::string joined) -> std::pair<std::string, std::string> {
  static constexpr std::string_view k_seam = "\nstack traceback:";
  const std::size_t at = joined.find(k_seam);
  if (at == std::string::npos) return {std::move(joined), {}};

  std::string frames = joined.substr(at + 1);  // past the newline
  joined.resize(at);
  return {std::move(joined), std::move(frames)};
}

// Pops a failed call's error value and turns it into an Error, keeping the
// traceback reachable on its own.
inline auto traced_error(core::State *L, const char *what) -> Error {
  auto [message, frames] = split_traceback(take_error(L));
  return Error(std::string(what) + ": " + message, std::move(frames));
}

}  // namespace detail

}  // namespace luakit
