// An owning handle to a Lua value, for C++ objects that outlive a stack slot.

#pragma once

#include "luakit/core/api.hpp"
#include "luakit/state.hpp"

#include <memory>
#include <utility>

namespace luakit {

// Lua values are garbage collected, so a C++ object cannot hold one by
// remembering a stack index: the slot is reused as soon as the call returns
// and the value collected once nothing else refers to it. luaL_ref solves this
// by moving the value into the registry -- which is a root -- and handing back
// an integer key. luaL_unref gives the key back.
//
// Move-only. Copying would end in two unrefs of one key, and the second would
// free a slot that luaL_ref has since handed to someone else, so two unrelated
// handles would silently alias.
//
// A Ref made from an Interpreter's state survives that Interpreter being
// closed: it takes a share of the control block (see state.hpp), and once that
// says closed there is no registry left to give a key back to, so releasing
// becomes a no-op instead of a use-after-free. Releasing early is still
// tidier, and still what a host should do, but forgetting is no longer fatal.
//
// A Ref made from a lua_State luakit did not create has no block to consult,
// and the older contract applies to it: that state must outlive the Ref.
class Ref {
 public:
  Ref() = default;

  // Anchors the value on top of the stack, popping it.
  static auto pop(core::State *L) -> Ref { return Ref(L); }

  // Anchors a copy of the value at idx, leaving the caller's stack untouched.
  static auto at(core::State *L, int idx) -> Ref {
    core::pushvalue(L, idx);
    return Ref(L);
  }

  ~Ref() { release(); }

  Ref(Ref &&other) noexcept
      : L_(std::exchange(other.L_, nullptr)),
        handle_(std::move(other.handle_)),
        key_(std::exchange(other.key_, core::NOREF)) {
    other.handle_.reset();
  }

  auto operator=(Ref &&other) noexcept -> Ref & {
    if (this != &other) {
      release();
      L_ = std::exchange(other.L_, nullptr);
      handle_ = std::move(other.handle_);
      other.handle_.reset();
      key_ = std::exchange(other.key_, core::NOREF);
    }
    return *this;
  }

  Ref(const Ref &) = delete;
  auto operator=(const Ref &) -> Ref & = delete;

  // False for a default-constructed or moved-from Ref. A Ref anchoring nil is
  // still valid -- it refers to something, and that something is nil.
  auto valid() const noexcept -> bool { return L_ != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

  // Pushes the value onto L and returns its type. L must belong to the same
  // interpreter the Ref was made from; a different thread of it is fine, since
  // the registry is shared across threads.
  //
  // An empty Ref pushes nil, as does one anchoring nil: luaL_ref answers a nil
  // value with REFNIL rather than allocating a slot for it.
  auto push(core::State *L) const -> int {
    if (key_ < 0) {
      core::pushnil(L);
      return core::TNIL;
    }
    return core::rawgeti(L, core::REGISTRYINDEX, key_);
  }

  // The state the value is anchored in.
  auto state() const noexcept -> core::State * { return L_; }

  // Whether the interpreter this was anchored in is still open. False once it
  // has been closed, which is what everything built on Ref checks before
  // touching the state.
  auto state_open() const noexcept -> bool { return L_ != nullptr && (!handle_ || handle_->open); }

  // Releases the anchor and leaves this empty. Idempotent.
  auto reset() noexcept -> void {
    release();
    L_ = nullptr;
    handle_.reset();
    key_ = core::NOREF;
  }

 private:
  // Order matters: handle_ is read out of the registry before aux::ref pops
  // the value being anchored. handle_of leaves the stack as it found it, so
  // the value is still on top when ref runs.
  explicit Ref(core::State *L) : L_(L), handle_(detail::handle_of(L)), key_(core::aux::ref(L, core::REGISTRYINDEX)) {}

  // luaL_unref already ignores NOREF and REFNIL, so what is guarded here is
  // the null state of a moved-from Ref, and the closed interpreter: once the
  // lua_State is gone there is no registry to give the key back to, and asking
  // would be the use-after-free this exists to prevent.
  auto release() noexcept -> void {
    if (!L_) return;
    if (handle_ && !handle_->open) return;
    core::aux::unref(L_, core::REGISTRYINDEX, key_);
  }

  core::State *L_ = nullptr;
  std::shared_ptr<StateHandle> handle_;
  int key_ = core::NOREF;
};

}  // namespace luakit
