#pragma once

#include "luakit/core/api.hpp"
#include "luakit/function.hpp"
#include "luakit/interpreter.hpp"
#include "luakit/stack.hpp"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace luakit {

enum class Status { suspended, finished };

// Drives a Lua coroutine from C++: resume it with typed arguments and read
// back what it yields.
//
// The lifetime problem this solves. A thread from lua_newthread is garbage
// collected, not closed. It survives only while Lua still references it, so
// holding nothing but the core::State* lets the collector free it underneath
// you -- a use-after-free that surfaces only once a collection happens to run.
// The constructor therefore anchors the thread in the registry with aux::ref
// and the destructor releases it with unref.
//
// Not supported: a bound C++ function yielding from inside a resume. That
// needs lua_yieldk and continuation functions, which is a different design.
class Coroutine {
 public:
  // Runs the function held in a global of the given name.
  Coroutine(Interpreter &host, const char *global_name) : host_(host.raw()) {
    core::getglobal(host_, global_name);
    if (!core::isfunction(host_, -1)) {
      core::pop(host_, 1);
      throw Error(std::string("luakit: global '") + global_name + "' is not a function");
    }
    adopt_top();
  }

  // Runs the function at a stack index of an existing state. The value is
  // copied, so the caller's stack is left untouched.
  static auto from_stack(core::State *L, int idx) -> Coroutine { return Coroutine(L, idx); }

  ~Coroutine() { release(); }

  Coroutine(Coroutine &&other) noexcept
      : host_(std::exchange(other.host_, nullptr)),
        co_(std::exchange(other.co_, nullptr)),
        ref_(std::exchange(other.ref_, core::NOREF)),
        finished_(std::exchange(other.finished_, true)) {}

  auto operator=(Coroutine &&other) noexcept -> Coroutine & {
    if (this != &other) {
      release();
      host_ = std::exchange(other.host_, nullptr);
      co_ = std::exchange(other.co_, nullptr);
      ref_ = std::exchange(other.ref_, core::NOREF);
      finished_ = std::exchange(other.finished_, true);
    }
    return *this;
  }

  Coroutine(const Coroutine &) = delete;
  auto operator=(const Coroutine &) -> Coroutine & = delete;

  // Resumes the coroutine, passing args as the function's parameters on the
  // first call and as the results of coroutine.yield() on later ones.
  //
  //   R = void  ->  bool, true while the coroutine is still suspended
  //   R = T     ->  std::optional<T>, nullopt once it has finished
  //
  // An error inside the coroutine throws Error, which also marks it finished.
  template <typename R = void, typename... Args>
  auto resume(Args &&...args) -> std::conditional_t<std::is_void_v<R>, bool, std::optional<R>> {
    if (done()) return stop<R>();

    (Stack<detail::stack_key_t<Args>>::push(co_, std::forward<Args>(args)), ...);

    int nresults = 0;
    const int st = core::resume(co_, host_, static_cast<int>(sizeof...(Args)), &nresults);

    if (st != core::OK && st != core::YIELD) {
      finished_ = true;
      throw Error("coroutine failed: " + take_message());
    }

    if (st == core::OK) {  // ran to completion
      finished_ = true;
      core::settop(co_, 0);
      return stop<R>();
    }

    if constexpr (std::is_void_v<R>) {
      core::settop(co_, 0);
      return true;
    } else {
      if (nresults < 1) {
        core::settop(co_, 0);
        throw Error("coroutine yielded no value");
      }
      // Deliberately test rather than check: a failed Stack<T>::check raises a
      // Lua error, and there is no protected call above us here to catch the
      // longjmp -- it would reach the panic handler and abort.
      const int first = -nresults;
      if (!Stack<R>::test(co_, first)) {
        const std::string got = core::aux::typename_(co_, first);
        core::settop(co_, 0);
        throw Error(std::string("coroutine yielded a ") + got + ", expected " + Stack<R>::name);
      }
      R out = Stack<R>::get(co_, first);
      // Lua requires the results be removed before the next resume.
      core::settop(co_, 0);
      return out;
    }
  }

  auto status() const noexcept -> Status { return done() ? Status::finished : Status::suspended; }

  // lua_status alone cannot tell "not started yet" from "ran to completion",
  // since both report LUA_OK, so completion is tracked here instead.
  auto done() const noexcept -> bool { return finished_ || co_ == nullptr; }

  // Escape hatch, for multiple yielded values and anything else not covered.
  auto raw() const noexcept -> core::State * { return co_; }

 private:
  Coroutine(core::State *L, int idx) : host_(L) {
    core::pushvalue(L, idx);
    if (!core::isfunction(L, -1)) {
      core::pop(L, 1);
      throw Error("luakit: value is not a function");
    }
    adopt_top();
  }

  // Takes the function on top of the host stack, creates the thread, anchors
  // it, and moves the function onto the thread's own stack ready to run.
  auto adopt_top() -> void {
    co_ = core::newthread(host_);                       // [fn, thread]
    ref_ = core::aux::ref(host_, core::REGISTRYINDEX);  // pops thread, anchors it
    core::xmove(host_, co_, 1);                         // fn moves to the thread
  }

  auto take_message() -> std::string {
    const char *err = core::gettop(co_) > 0 ? core::tostring(co_, -1) : nullptr;
    std::string msg = err ? err : "?";
    core::settop(co_, 0);
    return msg;
  }

  // A destructor must not throw, so closethread's status is ignored. Closing
  // first gives any pending <close> variables a deterministic run.
  auto release() noexcept -> void {
    if (ref_ == core::NOREF) return;
    if (co_) core::closethread(co_, host_);
    core::aux::unref(host_, core::REGISTRYINDEX, ref_);
    host_ = nullptr;
    co_ = nullptr;
    ref_ = core::NOREF;
  }

  template <typename R = void>
  static auto stop() -> std::conditional_t<std::is_void_v<R>, bool, std::optional<R>> {
    if constexpr (std::is_void_v<R>)
      return false;
    else
      return std::nullopt;
  }

  core::State *host_ = nullptr;  // must outlive this object: unref touches it
  core::State *co_ = nullptr;
  int ref_ = core::NOREF;
  bool finished_ = false;
};

}  // namespace luakit
