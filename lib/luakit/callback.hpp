// Function: a Lua function held by C++, so the host can call into a plugin.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/error.hpp"
#include "luakit/function.hpp"
#include "luakit/ref.hpp"
#include "luakit/stack.hpp"

#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace luakit {

namespace detail {

// result_count, result_borrows and read_results live in stack.hpp, so that
// Coroutine::resume and Interpreter::eval read a call's results by exactly the
// same rules this does.

// The interpreter's main thread, read out of the registry.
inline auto main_thread(core::State *L) noexcept -> core::State * {
  core::rawgeti(L, core::REGISTRYINDEX, core::RIDX_MAINTHREAD);
  core::State *m = core::tothread(L, -1);
  core::pop(L, 1);
  return m;
}

}  // namespace detail

// The other half of the binding. luakit::fn lets Lua call C++; this lets C++
// call Lua, which is what makes a plugin a plugin rather than a library:
//
//   auto on_tick(luakit::Function cb) -> void { handlers.push_back(std::move(cb)); }
//
//   // ... later, from the engine loop
//   for (const auto &h : handlers) h.call<void>(dt);
//
// It is an ordinary parameter type, so luakit::fn binds it like any other.
//
// Calls go through a protected call carrying the traceback handler, so a fault
// inside a plugin arrives as a luakit::Error with frames attached instead of
// taking the host down with it.
//
// Move-only, because Ref is: two handles releasing one registry key would free
// a slot that has since been handed to someone else.
class Function {
 public:
  Function() = default;

  // Anchors a copy of the function at idx, leaving the caller's stack alone.
  static auto at(core::State *L, int idx) -> Function { return Function(Ref::at(L, idx)); }

  // Anchors the function on top of the stack, popping it.
  static auto pop(core::State *L) -> Function { return Function(Ref::pop(L)); }

  auto valid() const noexcept -> bool { return ref_.valid(); }
  explicit operator bool() const noexcept { return valid(); }

  // Calls the function on the main thread and converts the result.
  //
  //   R = void         nothing is read back
  //   R = T            the first result, or an Error if it is not a T
  //   R = tuple<...>   that many results, in order
  //
  // An error inside the plugin, or a result of the wrong type, throws Error.
  template <typename R = void, typename... Args>
  auto call(Args &&...args) const -> R {
    return call_on<R>(main_, std::forward<Args>(args)...);
  }

  // Escape hatch: call on a specific thread rather than the main one.
  //
  // call() uses the main thread because a callback is usually registered from
  // inside whatever thread happened to be running at load time, and that
  // thread may well have finished by the time the host gets round to firing
  // it. The main thread is the one guaranteed still to be there.
  template <typename R = void, typename... Args>
  auto call_on(core::State *L, Args &&...args) const -> R {
    static_assert(!detail::result_borrows<R>,
                  "luakit: Function::call cannot return a borrowed type. The results come off the "
                  "stack before call returns, so a std::string_view or const char * would outlive "
                  "them. Use std::string.");

    if (!valid()) throw Error("luakit: call on an empty Function");
    if (!ref_.state_open()) throw Error("luakit: call on a Function whose interpreter has been closed");

    // Room for the function, its arguments, and the handler call_traced slips
    // underneath them. Reserving up front turns the one raise that is at all
    // likely here into a C++ exception instead.
    if (!core::checkstack(L, static_cast<int>(sizeof...(Args)) + 2)) {
      throw Error("luakit: cannot grow the Lua stack for a call");
    }

    const int base = core::gettop(L);
    detail::StackRestore restore(L, base);

    ref_.push(L);

    // Summed rather than assumed to be one per argument: a Stack<T> reports
    // how many values it pushed, and is entitled to push more than one.
    int nargs = 0;
    ((nargs += Stack<detail::stack_key_t<Args>>::push(L, std::forward<Args>(args))), ...);

    detail::arm_budget(L);
    if (detail::call_traced(L, nargs, detail::result_count<R>) != core::OK) {
      throw detail::traced_error(L, "luakit");
    }

    return detail::read_results<R>(L, base);
  }

  // Pushes the function onto L. Returns its type, or TNIL when empty.
  auto push(core::State *L) const -> int { return ref_.push(L); }

  // The thread call() will use.
  auto state() const noexcept -> core::State * { return main_; }

  auto reset() noexcept -> void {
    ref_.reset();
    main_ = nullptr;
  }

 private:
  explicit Function(Ref r) : main_(r.valid() ? detail::main_thread(r.state()) : nullptr), ref_(std::move(r)) {}

  core::State *main_ = nullptr;
  Ref ref_;
};

// A plain function value. Strict, like Stack<bool>: a table with a __call
// metamethod is callable but is not a function, and saying so is more use than
// accepting it and failing later somewhere less obvious.
template <>
struct Stack<Function> {
  static constexpr const char *name = "function";

  // False, and that is the point of the class: a Function anchors its value in
  // the registry, so unlike const char * or a userdata pointer it keeps
  // working long after the stack slot it came from is gone.
  static constexpr bool borrows = false;

  static auto test(core::State *L, int idx) noexcept -> bool { return core::isfunction(L, idx); }
  static auto check(core::State *L, int idx) -> void { core::aux::checktype(L, idx, core::TFUNCTION); }

  // On the contract: get() must not raise, and luaL_ref can, by failing to
  // grow the registry. That is an out-of-memory condition, and the same one
  // Userdata<T>::reserve documents; there is no allocation-free way to anchor.
  static auto get(core::State *L, int idx) -> Function { return Function::at(L, idx); }

  static auto push(core::State *L, const Function &f) -> int {
    f.push(L);
    return 1;
  }
};

}  // namespace luakit
