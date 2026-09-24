// The per-interpreter control block: what an Interpreter keeps beside its
// lua_State, and what everything holding a registry reference consults before
// giving one back.
//
// Its own header because Ref needs it and Interpreter needs Ref, so it cannot
// live in either.

#pragma once

#include "luakit/core/api.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace luakit {

// Resource limits for an interpreter running code the host did not write.
//
// Choosing which standard libraries a plugin sees (Interpreter::openlib) says
// what it may reach for. This says how much it may use, which is the other
// half: without it, `while true do end` in a mod hangs the host, and a script
// appending to a table in a loop takes the process down with an allocation
// failure the host never gets to see.
//
// Zero means unlimited, and is the default for both.
struct Limits {
  // Bytes of Lua-allocated memory. Enforced by the allocator, so exceeding it
  // surfaces as an ordinary Lua error ("not enough memory") that the host
  // catches as luakit::Error -- not as a crash.
  std::size_t memory = 0;

  // VM instructions per host-initiated call: one script(), one Function::call,
  // one Coroutine::resume. The budget is re-armed for each, so a handler that
  // is slow every frame is fine and a handler that never returns is not.
  unsigned long long instructions = 0;
};

// The block itself. Heap-allocated and shared, so it outlives the lua_State.
//
// That is the point of it. A Ref releases its registry key in its destructor,
// which touches the lua_State; hold one past lua_close -- a namespace-scope
// container of plugin callbacks, destroyed after main returns -- and that
// would be a use-after-free with nothing to detect it. The Interpreter marks
// the block closed *before* lua_close, and a Ref whose block is closed drops
// its key rather than giving it back. Releasing early is still tidier, and
// still what a host should do, but forgetting it cannot corrupt memory.
//
// It is also where the sandbox counters live: the allocator and the
// instruction hook each need somewhere per-state to keep a number, and this is
// the only per-state thing that exists before the state does.
struct StateHandle : std::enable_shared_from_this<StateHandle> {
  bool open = true;

  std::size_t used = 0;    // bytes currently allocated through our allocator
  std::size_t memory = 0;  // the cap, or 0

  unsigned long long instructions = 0;  // the per-call budget, or 0
  unsigned long long left = 0;          // what is left of it
};

namespace detail {

// A distinct address, used as the registry key the block is recorded under.
inline auto handle_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}

// The block for L, or null.
//
// Null is the ordinary answer for a lua_State luakit did not create -- a
// luaL_newstate in a test, or a host mixing luakit with hand-written C API
// code -- and everything that consults it falls back to the older behaviour,
// where the caller is responsible for ordering. Deliberately *not* read out of
// lua_getextraspace: Lua does not initialise that, so a foreign state hands
// back whatever was in the allocation and there is no way to tell.
inline auto handle_ptr(core::State *L) noexcept -> StateHandle * {
  if (core::rawgetp(L, core::REGISTRYINDEX, handle_key()) != core::TLIGHTUSERDATA) {
    core::pop(L, 1);
    return nullptr;
  }
  auto *raw = static_cast<StateHandle *>(core::touserdata(L, -1));
  core::pop(L, 1);
  return raw;
}

inline auto handle_of(core::State *L) noexcept -> std::shared_ptr<StateHandle> {
  StateHandle *raw = handle_ptr(L);
  return raw ? raw->shared_from_this() : nullptr;
}

// Records the block in L's registry. The registry is shared across an
// interpreter's threads, so a coroutine finds it too.
inline auto install_handle(core::State *L, StateHandle *h) -> void {
  core::pushlightuserdata(L, h);
  core::rawsetp(L, core::REGISTRYINDEX, handle_key());
}

// ------------------------------------------------------------- the cap
//
// Lua's allocator interface is also where a memory budget belongs: refusing an
// allocation is something Lua already knows how to survive. It unwinds and
// raises "not enough memory", which a protected call catches and the host sees
// as a luakit::Error -- rather than the abort a genuine malloc failure would
// be, deep inside whichever plugin happened to ask last.
//
// Accounting runs even when no cap is set, because it costs two additions and
// it is what Interpreter::memory_used reports.
inline auto limited_alloc(void *ud, void *ptr, std::size_t osize, std::size_t nsize) noexcept -> void * {
  auto *h = static_cast<StateHandle *>(ud);

  // When ptr is null Lua passes the *type* of the thing being allocated as
  // osize, not a size, so there is nothing to subtract.
  const std::size_t old = ptr ? osize : 0;

  if (nsize == 0) {
    h->used -= old;
    std::free(ptr);
    return nullptr;
  }

  if (h->memory != 0 && h->used - old + nsize > h->memory) return nullptr;

  void *fresh = std::realloc(ptr, nsize);
  if (!fresh) return nullptr;

  h->used = h->used - old + nsize;
  return fresh;
}

// What luaL_newstate installs, reproduced because lua_newstate does not.
// Reached only when something raises outside any protected call, which is a
// bug in the host rather than in a script.
inline auto panic(core::State *L) noexcept -> int {
  const char *msg = core::tostring(L, -1);
  std::fprintf(stderr, "luakit: unprotected error: %s\n", msg ? msg : "(non-string error)");
  std::fflush(stderr);
  return 0;  // returning from a panic function aborts
}

// ------------------------------------------------------- the other cap
//
// How often the count hook fires. Small enough that a runaway loop is stopped
// promptly, large enough that the check does not show up in a profile.
inline constexpr int k_hook_step = 1000;

inline auto instruction_hook(core::State *L, core::Debug *) -> void {
  StateHandle *h = handle_ptr(L);
  if (!h || h->instructions == 0) return;

  if (h->left > static_cast<unsigned long long>(k_hook_step)) {
    h->left -= static_cast<unsigned long long>(k_hook_step);
    return;
  }

  // Spent. Raising from a hook is allowed and is the only way to stop a script
  // that has no intention of returning. The budget is left at zero; the next
  // arm_budget refills it, so one runaway call does not disable the plugin.
  h->left = 0;
  core::aux::error(L, "luakit: instruction budget exhausted");
}

// Refills the budget and sets the hook, for one host-initiated call. Cheap
// enough to sit at the top of every call into Lua, and a no-op when no limit
// was asked for.
//
// Per call rather than per interpreter on purpose: a tick handler that is
// merely slow should keep working every frame, and only one that never returns
// should be stopped.
inline auto arm_budget(core::State *L) -> void {
  StateHandle *h = handle_ptr(L);
  if (!h || h->instructions == 0) return;

  h->left = h->instructions;
  core::sethook(L, &instruction_hook, core::MASKCOUNT, k_hook_step);
}

}  // namespace detail

}  // namespace luakit
