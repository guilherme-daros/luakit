#pragma once

#include "luakit/core/api.hpp"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace luakit {

// The name Lua knows T by. Every class exposed as userdata specializes this:
//
//   template <> struct luakit::Metatable<Widget> {
//     static constexpr const char *k_name = "app.Widget";
//   };
template <typename T>
struct Metatable;

// What a usable specialization has to provide. This cannot be a constraint on
// the declaration above: a constraint there would have to name Metatable<T>,
// which is the very name being declared. So the requirement is stated here
// and enforced where a metatable is consumed -- Userdata<T> below, and the
// Stack<T> / Stack<T *> / Stack<T &> specializations.
//
// Anything that converts to const char * is accepted, so `static constexpr
// char k_name[]` works as well as a pointer. The second requirement is not
// redundant: Stack<T>::name copies k_name into a constexpr member, so a
// k_name that is merely const would satisfy the conversion here and then fail
// much later, and much less legibly, as "not usable in a constant expression".
template <typename T>
concept Registered = requires {
  { Metatable<T>::k_name } -> std::convertible_to<const char *>;
  typename std::bool_constant<Metatable<T>::k_name != nullptr>;
};

// A field exposed on a registered class, so a script writes `entity.hp = 50`
// rather than `entity:set_hp(50)`.
//
// Only the plain struct lives here, because register_class below consumes it.
// The binders that produce one -- luakit::prop and luakit::accessor -- are in
// property.hpp, which would be a circular include from this file.
//
// A null `set` makes the field read-only, and assigning to it says so rather
// than quietly shadowing it with per-instance state.
struct PropertyReg {
  const char *name;
  core::CFunction get;
  core::CFunction set;
};

// Who is responsible for destroying the object behind a userdata.
//
// A host embedding Lua for plugins needs all three. The game creates most
// objects itself and merely lends them to a script -- an entity lives in the
// world, not in the collector -- while a script that builds its own objects
// wants them collected like anything else.
enum class Ownership : unsigned char {
  owned,     // constructed in place, destroyed by __gc
  borrowed,  // the host's object; __gc leaves it alone
  shared,    // a shared_ptr, whose count __gc drops
};

// The fixed header every userdata starts with, whatever it owns.
//
// `ptr` is what makes the modes interchangeable: it always points at the live
// object, so nothing downstream has to know which shape the allocation took.
// The payload, if there is one, follows this header -- an owned box carries
// the T itself, a shared box carries the shared_ptr, and a borrowed box
// carries nothing because the object is somewhere else entirely.
//
// `live` distinguishes a built object from raw memory. core::newuserdatauv
// hands back storage the collector already tracks, before any constructor has
// run, so __gc can reach a box that was never finished. It also makes double
// finalization a no-op.
//
// Untyped, so that a Derived box can be read through a Base's code path: the
// downcast in Userdata<T> has to reach `ptr` before it knows what T is.
struct BoxHeader {
  Ownership mode;
  bool live;
  void *ptr;
};

// Adds no members, which keeps Box<T> standard-layout and its header at offset
// zero -- the guarantee the cast above relies on.
template <typename T>
struct Box : BoxHeader {
  auto obj() const noexcept -> T * { return static_cast<T *>(ptr); }
};

// Marks the base classes of a registered type, so a Derived can be passed
// where a Base is expected:
//
//   template <> struct luakit::Metatable<Dog> {
//     static constexpr const char *k_name = "app.Dog";
//     using bases = luakit::Bases<Animal>;
//   };
//
// Bases must be registered before the classes deriving from them: registration
// copies their methods down and records how to convert a pointer.
template <typename... Bs>
struct Bases {};

namespace detail {

// The declared bases of T, or none.
template <typename T>
struct declared_bases {
  using type = Bases<>;
};
template <typename T>
  requires requires { typename Metatable<T>::bases; }
struct declared_bases<T> {
  using type = typename Metatable<T>::bases;
};

template <typename...>
struct cat_bases {
  using type = Bases<>;
};
template <typename... A>
struct cat_bases<Bases<A...>> {
  using type = Bases<A...>;
};
template <typename... A, typename... B, typename... Rest>
struct cat_bases<Bases<A...>, Bases<B...>, Rest...> : cat_bases<Bases<A..., B...>, Rest...> {};

template <typename T>
struct all_bases;

template <typename Bs>
struct expand_bases;

template <typename... Bs>
struct expand_bases<Bases<Bs...>> {
  // Each direct base, then everything that base in turn inherits. Duplicates
  // along a diamond are harmless: both entries say the same thing.
  using type = typename cat_bases<Bases<Bs>..., typename all_bases<Bs>::type...>::type;
};

template <typename T>
struct all_bases {
  using type = typename expand_bases<typename declared_bases<T>::type>::type;
};

// A distinct address per type, used as the key a cast is recorded under.
template <typename T>
auto type_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}

// Private fields kept in every class metatable, addressed by light userdata so
// that nothing a script writes can collide with them. Shared across all types,
// unlike type_key, because they name a slot rather than a type.
inline auto casts_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}
inline auto methods_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}
inline auto getters_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}
inline auto setters_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}
inline auto metamethods_key() noexcept -> const void * {
  static const char key = 0;
  return &key;
}

// Metamethods luakit owns, which a class may not supply and a derived class
// must not inherit.
//
// __gc is the one that would actually break: a base's finalizer installed on a
// derived class checks for the base's metatable, fails, and the object is
// never destroyed. __index and __newindex are what the field dispatch needs to
// do its job. (__name cannot reach here at all -- luaL_newmetatable sets it to
// the type name, and a Reg array can only hold functions.)
inline auto reserved_metamethod(const char *name) noexcept -> bool {
  return std::strcmp(name, "__gc") == 0 || std::strcmp(name, "__index") == 0 || std::strcmp(name, "__newindex") == 0;
}

// Adjusts a pointer from Derived to Base. The compiler does the work, so a
// base at a non-zero offset under multiple inheritance comes out right.
using Upcast = void *(*)(void *) noexcept;

template <typename Derived, typename Base>
auto upcast(void *p) noexcept -> void * {
  return static_cast<Base *>(static_cast<Derived *>(p));
}

}  // namespace detail

// Checked with static_assert rather than a requires-clause on the template.
// Constraining Userdata<T> would make the class vanish when T is unregistered,
// and every use of it downstream then fails on its own, burying the one error
// that matters under a page of unrelated ones.
template <typename T>
struct Userdata {
  static_assert(Registered<T>,
                "luakit: Metatable<T> must declare k_name as `static constexpr const char *`. "
                "Write, at namespace scope:\n"
                "  template <> struct luakit::Metatable<YourType> {\n"
                "    static constexpr const char *k_name = \"your.Name\";\n"
                "  };");

  using box_type = Box<T>;
  using shared_type = std::shared_ptr<T>;

  // Lua aligns every allocation to LUAI_MAXALIGN, which covers the fundamental
  // types and nothing beyond them -- in practice 8 bytes. An over-aligned T,
  // such as the alignas(16) vector types that turn up throughout game code,
  // would land on an address that does not satisfy alignof(T), and every
  // access through it from then on is undefined. Nothing at runtime would
  // report it, so it is caught here instead.
  static_assert(alignof(T) <= alignof(std::max_align_t),
                "luakit: T is over-aligned for Lua userdata storage. Lua only guarantees alignment "
                "suitable for the fundamental types. Lend such a type to Lua by pointer with "
                "push_ref, or give it a Stack<T> specialization that converts rather than boxing.");

  // Where the payload sits: directly after the header, rounded up to its own
  // alignment. Box<T> is standard-layout, so the header is at offset zero of
  // the allocation and this arithmetic is the usual trailing-object idiom.
  template <typename Payload>
  static constexpr std::size_t payload_offset =
      (sizeof(box_type) + alignof(Payload) - 1) / alignof(Payload) * alignof(Payload);

  template <typename Payload>
  static auto payload_of(box_type *b) noexcept -> Payload * {
    auto *base = reinterpret_cast<unsigned char *>(b);
    return reinterpret_cast<Payload *>(base + payload_offset<Payload>);
  }

  // Pushes GC-managed storage with the metatable attached, marked not-yet-
  // constructed. The caller placement-news into storage(b) and then calls
  // commit(b). Splitting this out lets a caller allocate *before* it
  // materialises any C++ argument, so nothing can be longjmp'd over.
  static auto reserve(core::State *L) -> box_type * {
    return new_box(L, Ownership::owned, payload_offset<T> + sizeof(T));
  }

  // Where reserve() expects the T to be constructed.
  static auto storage(box_type *b) noexcept -> void * { return payload_of<T>(b); }

  // Publishes a reserved box, once the T in storage(b) is fully built. The
  // userdata must be on top of the stack, where reserve() left it.
  //
  // live is set before the object is recorded, so that if recording raises the
  // box is already in a state __gc can finish correctly.
  static auto commit(core::State *L, box_type *b) -> T * {
    b->ptr = payload_of<T>(b);
    b->live = true;
    remember(L, b->obj());
    return b->obj();
  }

  // Takes a callable rather than constructor arguments: arguments are
  // evaluated at the call site, which would leave a C++ temporary alive across
  // core::newuserdatauv, and that longjmps on OOM.
  //
  // This can throw, so a hand-written core::CFunction calling it must be
  // wrapped in guard<>. Prefer luakit::ctor<T, Args...>, which is already.
  template <typename F>
  static auto emplace(core::State *L, F &&make) -> T * {
    box_type *b = reserve(L);
    new (storage(b)) T(make());
    return commit(L, b);
  }

  // Lends Lua an object the host owns. __gc will not touch it, so keeping it
  // alive for as long as a script can reach it is the host's problem -- which
  // is the deal any engine handing out a pointer to its own entity is already
  // making with itself.
  //
  // A null pointer pushes nil, so an absent thing reads as absent in Lua
  // rather than as an object that faults on first use.
  static auto push_ref(core::State *L, T *p) -> void {
    if (!p) {
      core::pushnil(L);
      return;
    }
    if (push_remembered(L, p)) return;

    box_type *b = new_box(L, Ownership::borrowed, sizeof(box_type));
    b->ptr = p;
    b->live = true;
    remember(L, p);
  }

  // Shares ownership, so neither side has to outlive the other.
  //
  // Taken by const reference and copied only after the allocation: a by-value
  // parameter would already hold a count by the time newuserdatauv ran, and
  // that count would be leaked if the allocation raised.
  static auto push_shared(core::State *L, const shared_type &sp) -> void {
    if (!sp) {
      core::pushnil(L);
      return;
    }
    if (push_remembered(L, sp.get())) return;

    box_type *b = new_box(L, Ownership::shared, payload_offset<shared_type> + sizeof(shared_type));
    new (payload_of<shared_type>(b)) shared_type(sp);
    b->ptr = sp.get();
    b->live = true;
    remember(L, b->obj());
  }

  // The T at idx, or null if the value is not one. Never raises, which is what
  // Stack<T *>::test needs.
  //
  // A Derived arrives here as a userdata with Derived's metatable, so the
  // exact-name check misses and the cast table is consulted. Nothing is looked
  // up in the common case, where the name matches on the first try.
  static auto try_get(core::State *L, int idx) noexcept -> T * {
    if (void *exact = core::aux::testudata(L, idx, Metatable<T>::k_name)) {
      auto *b = static_cast<BoxHeader *>(exact);
      return b->live ? static_cast<T *>(b->ptr) : nullptr;
    }
    if (core::type(L, idx) != core::TUSERDATA) return nullptr;

    auto *b = static_cast<BoxHeader *>(core::touserdata(L, idx));
    if (!b->live) return nullptr;

    detail::Upcast cast = find_upcast(L, idx);
    return cast ? static_cast<T *>(cast(b->ptr)) : nullptr;
  }

  // The T at stack index idx, or a Lua error if it is not one.
  static auto check(core::State *L, int idx) -> T * {
    if (T *p = try_get(L, idx)) return p;

    // Separate the two failures, because they call for different fixes: a
    // finalized object is a lifetime bug, a foreign one a call-site bug. The
    // box is only read once its metatable says it is one of ours, since an
    // unrelated userdata would be arbitrary bytes.
    if (core::type(L, idx) == core::TUSERDATA) {
      const bool ours = core::aux::testudata(L, idx, Metatable<T>::k_name) != nullptr || find_upcast(L, idx) != nullptr;
      if (ours && !static_cast<BoxHeader *>(core::touserdata(L, idx))->live) {
        core::aux::error(L, "%s used after finalization", Metatable<T>::k_name);
      }
    }
    core::aux::argexpected(L, false, idx, Metatable<T>::k_name);
    return nullptr;  // unreachable: argexpected raises
  }

  // Who owns the object at idx. Mostly of interest to a host deciding whether
  // it is safe to invalidate something.
  static auto ownership(core::State *L, int idx) -> Ownership {
    auto *b = static_cast<box_type *>(core::aux::checkudata(L, idx, Metatable<T>::k_name));
    return b->mode;
  }

  // Severs Lua's view of an object the host is about to destroy.
  //
  // The deal a borrowed box makes is that the host keeps the object alive for
  // as long as a script can reach it. Sometimes the host cannot: an entity
  // dies mid-frame while a mod still holds it. This is the way out. The
  // userdata standing for p is marked dead, so touching it afterwards raises
  // "used after finalization" rather than reading freed memory, and the cache
  // entry goes, so the address can be reused without inheriting the old box.
  //
  // Only borrowed boxes are severed. An owned box holds the object inside
  // itself, and marking it dead would skip the destructor; a shared box has a
  // count that keeps the object alive regardless.
  static auto invalidate(core::State *L, T *p) -> void {
    if (!p) return;
    push_cache(L);  // [cache]

    if (core::rawgetp(L, -1, p) == core::TUSERDATA) {  // [cache, ud]
      auto *b = static_cast<box_type *>(core::touserdata(L, -1));
      if (b->mode == Ownership::borrowed) {
        b->live = false;
        b->ptr = nullptr;
      }
    }
    core::pop(L, 1);  // [cache]

    core::pushnil(L);
    core::rawsetp(L, -2, p);
    core::pop(L, 1);
  }

  // A finalizer must never raise.
  static auto gc(core::State *L) noexcept -> int {
    auto *b = static_cast<box_type *>(core::aux::checkudata(L, 1, Metatable<T>::k_name));
    if (!b->live) return 0;
    b->live = false;

    switch (b->mode) {
      case Ownership::owned:
        b->obj()->~T();
        break;
      case Ownership::shared:
        std::destroy_at(payload_of<shared_type>(b));
        break;
      case Ownership::borrowed:
        break;  // the host's object, and none of our business
    }

    b->ptr = nullptr;
    return 0;
  }

  // Builds the metatable: the metamethods `meta` names, whatever the bases
  // contribute, and the __gc / __index / __newindex trio luakit owns.
  //
  // Metamethods inherit alongside methods, so a __tostring on a base is what a
  // derived class prints with. The three reserved names are the exception:
  // they are set last here and win over anything supplied or inherited, since
  // a base's __gc on a derived class would fail its own type check and the
  // field dispatch needs both index metamethods to work at all.
  static auto register_class(core::State *L, const core::aux::Reg *methods, const core::aux::Reg *meta = nullptr,
                             const PropertyReg *props = nullptr) -> void {
    core::aux::newmetatable(L, Metatable<T>::k_name);
    const int mt = core::gettop(L);

    // A null methods array is allowed, like a null meta or props: a class can
    // exist purely to be inherited from, or carry nothing but fields.
    if (methods) {
      core::aux::newlib(L, methods);
    } else {
      core::newtable(L);
    }
    core::newtable(L);
    core::newtable(L);
    core::newtable(L);

    const Tables own{mt + 1, mt + 2, mt + 3, mt + 4};

    for (const PropertyReg *p = props; p && p->name; ++p) {
      if (p->get) {
        core::pushcfunction(L, p->get);
        core::setfield(L, own.getters, p->name);
      }
      if (p->set) {
        core::pushcfunction(L, p->set);
        core::setfield(L, own.setters, p->name);
      }
    }

    for (const core::aux::Reg *m = meta; m && m->name; ++m) {
      if (detail::reserved_metamethod(m->name)) continue;
      core::pushcfunction(L, m->func);
      core::setfield(L, own.metamethods, m->name);
    }

    inherit_from_bases(L, own);
    record_casts(L, mt);

    // Kept so a class deriving from this one can copy them down. Under light
    // userdata keys, which nothing written from Lua can collide with.
    core::pushvalue(L, own.methods);
    core::rawsetp(L, mt, detail::methods_key());
    core::pushvalue(L, own.getters);
    core::rawsetp(L, mt, detail::getters_key());
    core::pushvalue(L, own.setters);
    core::rawsetp(L, mt, detail::setters_key());
    core::pushvalue(L, own.metamethods);
    core::rawsetp(L, mt, detail::metamethods_key());

    // Unconditional, so re-registering a class -- which is what hot reload
    // does -- really does replace what was there before.
    copy_all(L, mt, own.metamethods);

    core::pushcfunction(L, gc);
    core::setfield(L, mt, "__gc");

    core::pushvalue(L, own.methods);
    core::pushvalue(L, own.getters);
    core::pushcclosure(L, index_dispatch, 2);
    core::setfield(L, mt, "__index");

    core::pushvalue(L, own.setters);
    core::pushvalue(L, own.getters);
    core::pushcclosure(L, newindex_dispatch, 2);
    core::setfield(L, mt, "__newindex");

    core::settop(L, mt - 1);
  }

 private:
  // ---------------------------------------------------------- inheritance
  //
  // Members are copied down at registration rather than chained through a
  // metatable at lookup. Flattening keeps every access a single rawget however
  // deep the hierarchy is, and it handles multiple bases without needing a
  // walker: each base is merged in turn, nearest first, and a name already
  // present is left alone so an override wins.
  //
  // The cost is that adding a method to a base after the fact does not reach
  // classes already registered. Nothing does that.

  // The four tables a class is assembled from, by stack index.
  struct Tables {
    int methods;
    int getters;
    int setters;
    int metamethods;
  };

  // Copies every entry of src into dst, overwriting what is there.
  static auto copy_all(core::State *L, int dst, int src) -> void {
    core::pushnil(L);
    while (core::next(L, src)) {  // [key, value]
      core::pushvalue(L, -2);
      core::pushvalue(L, -2);
      core::rawset(L, dst);
      core::pop(L, 1);  // [key]
    }
  }

  // Copies anything from base `src` that `dst` does not already define.
  static auto merge_missing(core::State *L, int dst, int src) -> void {
    core::pushnil(L);
    while (core::next(L, src)) {  // [key, value]
      core::pushvalue(L, -2);     // [key, value, key]
      const bool present = core::rawget(L, dst) != core::TNIL;
      core::pop(L, 1);
      if (!present) {
        core::pushvalue(L, -2);  // [key, value, key]
        core::pushvalue(L, -2);  // [key, value, key, value]
        core::rawset(L, dst);
      }
      core::pop(L, 1);  // [key]
    }
  }

  // Pulls one base's table out of its metatable, if it registered one.
  static auto push_base_table(core::State *L, const char *base_name, const void *key) -> bool {
    if (core::aux::getmetatable(L, base_name) != core::TTABLE) {
      core::pop(L, 1);
      return false;
    }
    if (core::rawgetp(L, -1, key) != core::TTABLE) {
      core::pop(L, 2);
      return false;
    }
    core::remove(L, -2);  // drop the base metatable, keep the table
    return true;
  }

  template <typename B>
  static auto merge_one_base(core::State *L, const Tables &own) -> void {
    const struct {
      int dst;
      const void *key;
    } slots[] = {
        {own.methods,     detail::methods_key()    },
        {own.getters,     detail::getters_key()    },
        {own.setters,     detail::setters_key()    },
        {own.metamethods, detail::metamethods_key()},
    };

    for (const auto &slot : slots) {
      if (!push_base_table(L, Metatable<B>::k_name, slot.key)) continue;
      merge_missing(L, slot.dst, core::gettop(L));
      core::pop(L, 1);
    }
  }

  // maybe_unused because a class with no bases expands this fold to nothing.
  template <typename... Bs>
  static auto merge_bases([[maybe_unused]] core::State *L, [[maybe_unused]] const Tables &own, Bases<Bs...> *) -> void {
    (merge_one_base<Bs>(L, own), ...);
  }

  static auto inherit_from_bases(core::State *L, const Tables &own) -> void {
    using bases = typename detail::all_bases<T>::type;
    merge_bases(L, own, static_cast<bases *>(nullptr));
  }

  // Records how to turn a T* into each of its base pointers, so a Derived can
  // be passed where a Base is expected. static_cast does the arithmetic, which
  // is what makes a base at a non-zero offset come out right.
  template <typename... Bs>
  static auto write_casts([[maybe_unused]] core::State *L, [[maybe_unused]] int table, Bases<Bs...> *) -> void {
    (
        [&] {
          core::pushlightuserdata(L, const_cast<void *>(detail::type_key<Bs>()));
          core::pushlightuserdata(L, reinterpret_cast<void *>(&detail::upcast<T, Bs>));
          core::rawset(L, table);
        }(),
        ...);
  }

  static auto record_casts(core::State *L, int mt) -> void {
    using bases = typename detail::all_bases<T>::type;
    core::newtable(L);
    const int casts = core::gettop(L);
    write_casts(L, casts, static_cast<bases *>(nullptr));
    core::rawsetp(L, mt, detail::casts_key());
  }

  // Looks for a recorded conversion from the value's own class to T.
  //
  // The function pointer travels as a light userdata, which needs a cast
  // between object and function pointers. That is conditionally supported by
  // the standard and required to work by POSIX, which is every platform an
  // embedded Lua runs on.
  static auto find_upcast(core::State *L, int idx) noexcept -> detail::Upcast {
    if (!core::getmetatable(L, idx)) return nullptr;
    if (core::rawgetp(L, -1, detail::casts_key()) != core::TTABLE) {
      core::pop(L, 2);
      return nullptr;
    }

    detail::Upcast out = nullptr;
    if (core::rawgetp(L, -1, detail::type_key<T>()) == core::TLIGHTUSERDATA) {
      out = reinterpret_cast<detail::Upcast>(core::touserdata(L, -1));
    }
    core::pop(L, 3);
    return out;
  }

  // ------------------------------------------------------ field dispatch
  //
  // Both of these are C closures rather than plain tables, because a lookup
  // now has three places to go: the methods, the properties, and whatever the
  // script itself stashed on the object.
  //
  // That last one is the uservalue slot every box reserves. Letting a mod
  // write `entity.my_flag = true` and read it back costs one table per object
  // that is actually written to, and saves every plugin author from inventing
  // a side table keyed by entity.

  // Upvalue 1: the method table. Upvalue 2: the property getters.
  static auto index_dispatch(core::State *L) -> int {
    core::pushvalue(L, 2);
    if (core::rawget(L, core::upvalueindex(1)) != core::TNIL) return 1;
    core::pop(L, 1);

    core::pushvalue(L, 2);
    if (core::rawget(L, core::upvalueindex(2)) != core::TNIL) {
      core::pushvalue(L, 1);  // self
      core::call(L, 1, 1);    // errors inside a getter propagate, as they should
      return 1;
    }
    core::pop(L, 1);

    if (core::getiuservalue(L, 1, 1) == core::TTABLE) {
      core::pushvalue(L, 2);
      core::rawget(L, -2);
      return 1;
    }
    core::pop(L, 1);

    core::pushnil(L);
    return 1;
  }

  // Upvalue 1: the property setters. Upvalue 2: the getters, consulted only to
  // tell a read-only field from an unknown one.
  static auto newindex_dispatch(core::State *L) -> int {
    core::pushvalue(L, 2);
    if (core::rawget(L, core::upvalueindex(1)) != core::TNIL) {
      core::pushvalue(L, 1);  // self
      core::pushvalue(L, 3);  // value
      core::call(L, 2, 0);
      return 0;
    }
    core::pop(L, 1);

    core::pushvalue(L, 2);
    if (core::rawget(L, core::upvalueindex(2)) != core::TNIL) {
      // Silently shadowing a read-only field with per-instance state would
      // make the next read return the assignment and look like it worked.
      return core::aux::error(L, "%s: field '%s' is read-only", Metatable<T>::k_name, core::tostring(L, 2));
    }
    core::pop(L, 1);

    if (core::getiuservalue(L, 1, 1) != core::TTABLE) {
      core::pop(L, 1);  // whatever was there, usually nil
      core::createtable(L, 0, 1);
      core::pushvalue(L, -1);
      core::setiuservalue(L, 1, 1);
    }
    core::pushvalue(L, 2);
    core::pushvalue(L, 3);
    core::rawset(L, -3);
    return 0;
  }

  // The one allocation point. Everything is set except ptr and live, which the
  // caller fills in once it has something real to point at -- until then the
  // box has to survive a collection, and live == false is what lets it.
  //
  // One uservalue, which is where index_dispatch keeps per-instance state.
  static auto new_box(core::State *L, Ownership mode, std::size_t size) -> box_type * {
    auto *b = static_cast<box_type *>(core::newuserdatauv(L, size, 1));
    b->mode = mode;
    b->live = false;
    b->ptr = nullptr;
    core::aux::setmetatable(L, Metatable<T>::k_name);
    return b;
  }

  // ------------------------------------------------------- identity cache
  //
  // One userdata per object, rather than one per push. Without this, handing
  // the same entity to a script twice produces two unrelated Lua values: `a ==
  // b` is false, and a field a mod set on one is missing from the other. Both
  // are the kind of bug that only shows up in someone else's plugin.
  //
  // Values are weak, so an entry disappears along with the userdata it names
  // and the table cannot pin objects alive. Lua clears weak values before
  // running finalizers, so __gc never has to tidy up after itself here.
  //
  // Keys are the raw pointer as a light userdata. An address the host frees
  // and reallocates is therefore the one thing this cannot see through, which
  // is what invalidate() is for.

  // A distinct address per T, used as this type's registry key. Nothing is
  // ever read through it.
  static auto cache_key() noexcept -> const void * {
    static const char key = 0;
    return &key;
  }

  // Pushes this type's cache, creating it on first use.
  static auto push_cache(core::State *L) -> void {
    if (core::rawgetp(L, core::REGISTRYINDEX, cache_key()) == core::TTABLE) return;
    core::pop(L, 1);  // the nil

    core::createtable(L, 0, 8);

    core::createtable(L, 0, 1);
    core::pushstring(L, "v");
    core::setfield(L, -2, "__mode");
    core::setmetatable(L, -2);

    core::pushvalue(L, -1);
    core::rawsetp(L, core::REGISTRYINDEX, cache_key());
  }

  // Pushes the userdata already standing for p, if there is one.
  static auto push_remembered(core::State *L, T *p) -> bool {
    push_cache(L);                                     // [cache]
    if (core::rawgetp(L, -1, p) == core::TUSERDATA) {  // [cache, ud]
      core::remove(L, -2);                             // [ud]
      return true;
    }
    core::pop(L, 2);  // drop the nil and the cache
    return false;
  }

  // Records the userdata on top of the stack as standing for p, and leaves it
  // on top.
  static auto remember(core::State *L, T *p) -> void {
    push_cache(L);           // [ud, cache]
    core::pushvalue(L, -2);  // [ud, cache, ud]
    core::rawsetp(L, -2, p);
    core::pop(L, 1);  // [ud]
  }
};

}  // namespace luakit
