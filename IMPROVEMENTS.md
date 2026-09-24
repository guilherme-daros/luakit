# luakit — improvement plan

**Status: implemented.** Everything below was carried out; this file is kept as
the record of what was found, what was done about it, and the few things
deliberately left alone. Verified numbers and outputs are from the working tree
at the time of the change.

Summary of what landed:

| Area | Outcome |
| --- | --- |
| Base-pointer identity | Fixed; most-derived-type resolution, RTTI-gated |
| `invalidate` cache eviction | Fixed; only a severed borrow loses its entry |
| `T &` returns, `const T *`/`const T &`, `unique_ptr` | All now bind |
| `Stack<Ref>`, `set`/`unordered_set` | Added |
| Handles outliving the interpreter | No longer UB; they report it |
| Sandboxing | Memory cap, instruction budget, restricted `_ENV` |
| LuaLS definitions | `--emit-defs`, checked into `lua/defs/`, CI-verified |
| GC control, `eval<T>`, `package_path`, `on_print`, `Table::for_each` | Added |
| Tests | 13 suites → 17, plus 7 compile-fail tests |
| CI | Added: 2 compilers x 2 build types, no-RTTI, install-consumer, format |

One thing was found by the new benchmark rather than by reading, and is
recorded at the bottom.

---

## What was found

Written after a full read of `lib/`, `src/`, `test/`, `lua/`, `tools/` and the
CMake layer, at `9c6d91c` plus the uncommitted `openlibs()` move. The build is
clean and all 13 suites pass under ASan/UBSan.

Everything in "Verified" below was reproduced with a throwaway program, not
inferred from reading. Everything in "Proposed" is a design opinion.

## What is already right, and should not be traded away

Three things make this better than most hand-rolled binding layers, and every
proposal below is constrained by them:

- **The four-phase `Stack<T>` contract.** `test` / `check` / `get` / `push`
  with the longjmp boundary between `check` and `get` is the correct answer to
  the hardest problem in Lua binding, and the codebase actually honours it
  everywhere (`ctor_call` reserving the box before materialising arguments is
  the sharpest example).
- **`borrows` as a compile-time property that propagates through composites**,
  with `Function::call`, `Coroutine::resume` and `Table::get` refusing borrowed
  results. That is a whole class of dangling-view bugs deleted at compile time.
- **Per-object ownership rather than per-class**, plus the weak identity cache.
  Getting `a == b` and per-instance state right for a lent pointer is exactly
  what a modding layer needs and is routinely got wrong.

The comments are unusually good — they explain *why*, and they are accurate.
Keep that bar.

---

## Part 1 — Correctness gaps (verified)

### 1.1 Returning a base pointer loses the derived identity *(highest impact)*

```cpp
auto as_base()    -> Base *    { return &d; }   // d is really a Derived
auto as_derived() -> Derived * { return &d; }
```

```
identity x==y:                              false
x.b (derived field through base return):    nil
y.b:                                        2
tag visible on y:                           nil
```

Passing a `Derived` **into** a `Base` parameter works (the cast table handles
it, and `test_class.cpp` covers it). Returning `Base *` **out** does not:
`Stack<Base *>::push` calls `Userdata<Base>::push_ref`, whose identity cache is
keyed under `Userdata<Base>::cache_key()`, a different registry slot from
`Userdata<Derived>`'s. The result is two unrelated Lua objects for one C++
object — different metatables, no shared per-instance state, `==` false.

This is the single most consequential gap, because an engine API is *mostly*
base pointers: `Entity *find(...)`, `Entity *parent()`, `std::vector<Entity *>
children()`. A mod that stashes state on an entity obtained one way cannot see
it the other way.

**Fix.** Give `push_ref` a most-derived-type resolution step for polymorphic
`T`:

- At `register_class`, record `typeid(T).name()` (or the `std::type_index`
  address) → a "push as this type" thunk, in a registry-wide table.
- In `Userdata<T>::push_ref`, when `std::is_polymorphic_v<T>`, look up
  `typeid(*p)`; on a hit, `dynamic_cast` to the most-derived registered type
  and delegate to *its* `push_ref`. One registry lookup on the miss path,
  nothing at all for non-polymorphic `T`.
- The identity cache then has one entry, under the derived type, and
  `Stack<Base *>::get` already finds it through the existing upcast table.

Cost: one `rawgetp` plus a `dynamic_cast` per *first* push of an object; cached
pushes are unaffected. Gate it on `std::is_polymorphic_v<T>` so `Vec2` pays
nothing. Document that an unregistered derived type falls back to the static
type, which is today's behaviour.

### 1.2 `invalidate` erases the identity entry for owned and shared boxes too

`Userdata<T>::invalidate` only marks `borrowed` boxes dead — correct — but the
`rawsetp(nil)` that drops the cache entry runs unconditionally. Calling it on
an owned or shared object leaves the object alive and reachable while its
identity entry is gone, so the next push mints a second userdata: `==` breaks
and stashed fields vanish, silently.

**Fix.** Move the cache eviction inside the `mode == borrowed` branch, and
document that `invalidate` on a non-borrowed object is a no-op. One-line
change, one test.

### 1.3 `method<M>` accepts a data member and fails with a template wall

`prop<>` and `ro_prop<>` have good `static_assert`s for the inverse mistake.
`method<&T::field>` produces three screens of "incomplete type
`signature<int W::*>`". Add the mirror assertion in
`detail::signature`/`method_impl`: *"method<> takes a pointer to a member
function; for a data member use prop<> or ro_prop<>."*

### 1.4 `tuple` means different things in `Function::call` and `Coroutine::resume`

`Function::call<std::tuple<int, int>>` reads **two Lua results**, by the
documented special case. `Coroutine::resume<std::tuple<int, int>>` goes through
`Stack<std::tuple<...>>`, i.e. `FixedSequence`, and expects **one yielded
table**. Two spellings, one type, opposite meanings — and `resume` is the one
that has no other way to read a multi-value yield (today you drop to `raw()`).

**Fix.** Make `resume` use the same `result_count` / tuple-as-multiple-results
rule `Function::call` uses, and say so in `README`. This is a behaviour change,
so it wants a note in the header comment.

### 1.5 The chaining check refuses a legitimate `T &` return

`method_call` raises *"method returned a different object than self"* whenever
a `C &`-returning method hands back anything but the receiver. But
`Entity &World::at(int)` and `Node &Node::child(int)` are ordinary APIs, and
`push_ref` would handle them correctly (identity cache included).

**Fix.** Keep the fast path — if `returned == self`, `settop(L, 1)` and return
the receiver already on the stack — and fall through to
`Stack<C &>::push(L, *returned)` otherwise, instead of raising.

### 1.6 String conversion is lax where `bool` is strict

`Stack<std::string>` and `Stack<const char *>` use `isstring`, which is true for
numbers, so `greet(42)` succeeds. The header argues at length that `Stack<bool>`
is strict *because* the error message beats silent acceptance — the same
argument applies here, and the `luaL_checkstring` precedent cuts the other way.

Not obviously a bug, but it *is* an unexamined inconsistency. Either document
it beside the `Stack<bool>` comment, or add an opt-in strict string type. My
preference: document it; matching `luaL_checkstring` is defensible and changing
it now would break scripts.

---

## Part 2 — Type coverage gaps (verified compile failures)

These three all fail to compile today, and all three are shapes an engine API
produces constantly.

| Shape | Today | Proposal |
| --- | --- | --- |
| `const T *` parameter | `static_assert`: no `Stack<T>` for this type | strip cv in `stack_key_t` for registered pointers; bind as `T *` |
| `const T &` return | "cannot bind non-const lvalue reference" | add a `Stack<const T &>` that pushes via `push_ref` on a `const_cast` |
| `std::unique_ptr<T>` return | `static_assert`: no `Stack<T>` | new specialization: move into an owned box, Lua's collector destroys it |

- **`const T *` / `const T &`.** A `const Entity *find() const` is the natural
  spelling and is currently unbindable, which quietly pushes hosts toward
  dropping `const` from their own API to satisfy the binding layer. Lua has no
  const, so the honest implementation is to erase it at the boundary and say so
  in the header: *"const is not carried into Lua; a script can call any bound
  mutator on a `const T *` it was handed."* That caveat is worth one paragraph
  and is much better than the status quo.
- **`std::unique_ptr<T>`.** This is the missing fourth row of the ownership
  table: *transfer*. Today handing Lua an object the host built and no longer
  wants means returning `T` by value (needs copy/move and an extra copy) or
  `shared_ptr` (needs a control block nobody wanted). `unique_ptr` maps exactly
  onto `Ownership::owned`: move-construct into the box, `__gc` destroys.
  Implementation is ~20 lines next to `Stack<std::shared_ptr<T>>`.

Also missing and cheap:

- **`Stack<Ref>`.** `Table` and `Function` both have one; `Ref` — the "any Lua
  value, anchored" type — does not, so a bound function cannot accept an
  arbitrary value and keep it. `on_event(std::string, luakit::Ref)` is a
  three-line specialization away and completes the set.
- **`std::set` / `std::unordered_set`** as sequence tables, and
  **`std::vector<bool>`** (which currently instantiates `Stack<T>::get`
  returning into a proxy — worth a test either way).

---

## Part 3 — Features worth building

Ordered by how much they serve the README's own thesis ("a plugin or modding
layer to a program that already exists").

### 3.1 Sandboxing: resource limits for untrusted mods *(biggest missing feature)*

`openlib(Lib::string)` lets a host choose *which* libraries a plugin sees. What
it cannot do is bound what a plugin *does*. A mod with `while true do end`
hangs the host; a mod that appends to a table in a loop takes the process down
with `abort()` on OOM. For a modding layer that is the difference between "my
game crashed" and "plugin X was disabled".

Propose a `luakit::Sandbox` (or options on `Interpreter`):

```cpp
luakit::Interpreter lua({
    .memory_limit = 64 << 20,          // via a custom lua_Alloc
    .instruction_limit = 10'000'000,   // via lua_sethook(LUA_MASKCOUNT)
});
```

- **Memory cap** — `lua_newstate` with an allocator that refuses past a budget.
  Lua handles allocation failure as a normal error, so this degrades into a
  catchable `luakit::Error` rather than a crash. Needs `Interpreter` to stop
  using `luaL_newstate`, and a per-state control block to hold the counter.
- **Instruction cap** — a count hook that raises after N VM instructions,
  armed per protected call and disarmed after. Gives a host a real answer to a
  runaway `on_tick`.
- **Restricted env** — a helper that builds a `_ENV` table exposing a named
  allowlist, so a plugin gets `string`/`table`/`math` but not `os.execute` or
  `package.loadlib`, without the host hand-rolling it.

This is the one feature whose absence limits what the library is *for*.

### 3.2 A per-state control block, and the end of the "must not outlive the
Interpreter" rule

`Ref`, `Function`, `Table` and `Coroutine` all document the same hazard: hold
one past `lua_close` and the destructor unrefs against a freed `lua_State`.
`README` spends a paragraph on it, `world::shutdown()` exists to work around
it, and the demo needs a `WorldSession` guard so an early return cannot skip
it. That is a lot of ceremony for a footgun that can be removed.

`lua_getextraspace` is already exposed. Put a `std::shared_ptr<StateInfo>`
there at `Interpreter` construction; `Ref` captures a `weak_ptr` alongside its
key; `Interpreter`'s destructor marks the block dead *before* `lua_close`. A
`Ref` whose block is dead simply drops its key instead of unref'ing. The
static-destruction-order bug becomes impossible, the README paragraph becomes
one sentence, and `world::shutdown()` shrinks to housekeeping.

This also gives a natural home for the sandbox counters in 3.1 and for the
"which `Interpreter` am I in" question `world.cpp`'s `host_state` global is
currently answering by hand — `invalidate` could then take an `Interpreter &`.

### 3.3 Generate LuaLS type definitions from the registration

This is the highest-leverage *new* feature, and the repo is already most of the
way to it in spirit: `luakit_embed_dir`'s `STUB_DIR` exists purely so a C++ LSP
has something to resolve against before the first configure. Mod authors
deserve the same on the Lua side.

`Class<T>` and `Library` already hold every name, and the C++ signatures are
available at the point of registration. Add a `--dump-api` mode (a
`LUAKIT_EMIT_DEFS` build of the host, or a `build_module()` variant that writes
instead of registers) emitting `---@meta` definitions:

```lua
---@class world.Creature : world.Entity
---@field hp integer
---@field facing "north"|"south"|"east"|"west"
local Creature = {}
---@param amount integer
---@return world.Creature
function Creature:damage(amount) end
```

Enums become string literal unions — which is exactly what `EnumNames` already
knows and what makes `goblin.facing = "nrth"` catchable in the *editor* rather
than at the call. Inheritance becomes `---@class A : B`, which `Bases<>`
already knows. Property read-onlyness is already in `PropertyReg`.

Payoff: autocomplete, type checking and inline docs for every mod author, from
information the library already has, with no annotation burden on the host.

### 3.4 GC control from the host

A game loop wants to say "run at most 2 ms of collection this frame". There is
no `Interpreter::gc(...)` at all today — no `collect`, `step`, `count`,
`setpause`/`setstepmul`, and no way to switch Lua 5.4's generational mode on.
`lua_gc` is already aliased in `core::`; this is a thin, obvious, high-value
wrapper:

```cpp
lua.gc().step(200);              // KB of work
const std::size_t kb = lua.gc().count();
lua.gc().generational(20, 100);
```

### 3.5 Smaller, but each removes a real friction point

- **`Interpreter::eval<T>(code)`** — run a chunk and read its return value.
  Loading a config file that ends in `return { ... }` currently needs
  `load_script_as_module` plus hand-written stack code. `script()` returning
  `void` is the wrong default for half the cases.
- **`Interpreter::package_path(...)`** — a host loading mods from `mods/`
  must reach past the facade into `package.path` today.
- **Output redirection** — scripts `print` to stdout; a game wants it in its
  own console. A `Interpreter::on_print(std::function<void(std::string_view)>)`
  that replaces the global is ten lines and universally wanted.
- **`Table` iteration** — `count()` is there but there is no `for_each`, so
  reading a config table with unknown keys means dropping to `lua_next`.
- **Better overload diagnostics** — *"no overload matches the 2 argument(s)
  given"* should list the candidate signatures; every arm already knows its
  `Stack<A>::name`s.
- **`Error::traceback()`** — the traceback is concatenated into `what()`;
  keeping it as a separate member lets a host log one and show the other.
- **`Class<T>::enum_<E>("Facing")`** — put the enum's names in the module table
  so `world.Facing.north` is discoverable from Lua, instead of the valid
  strings being knowable only by triggering an error.
- **Message prefix consistency** — `guard` emits `[guard] `, everything else
  emits `luakit: `.

---

## Part 4 — Infrastructure

- **There is no CI.** `format-check`, 13 sanitized suites and a demo all exist
  and nothing runs them on a push. A GitHub Actions matrix over
  {GCC, Clang} × {Debug, Release} running `ctest` plus `format-check`, and one
  job that builds `LUNA_EMBED=bytecode` (the only configuration that exercises
  `luac` + `xxd`), is a day's work and protects everything above.
- **Header self-containment is claimed but not enforced.** README: *"Each
  header stands on its own."* Add a generated target that compiles a one-line
  TU per public header. Cheap, and it is exactly the invariant that rots.
- **Warnings only reach the tests.** `-Wall -Wextra -pedantic` is set in
  `test/CMakeLists.txt` only; `luna_demo` and the library's interface get
  nothing. Move it to a shared `luakit_warnings` interface target and apply it
  to the demo too. Consider `-Wconversion` for the library — a binding layer is
  where narrowing bugs live.
- **No compile-fail tests.** Several of the best safety properties are
  `static_assert`s — `borrows` refusal in `Table::get`, variadic-last,
  over-alignment, missing `Metatable<T>`. None is tested, so any of them could
  be silently weakened. A small CMake helper doing `try_compile` and asserting
  failure plus a message match would lock them in.
- **No benchmarks.** "A lookup costs the same however deep the hierarchy is" is
  a design claim with no measurement behind it. A micro-benchmark (call
  overhead, property get/set, push/get round trip, inheritance depth) would
  both back the claim and catch regressions from 1.1's `dynamic_cast` path.
- **No `luakit/version.hpp`.** CMake knows 0.1.0; a consumer using
  `find_package` cannot `#if LUAKIT_VERSION >= ...`.

---

## Part 5 — Documentation fixes

- **The hot-reload paragraph is wrong about objects.** README says *"objects
  built from the previous metatable all keep the old behaviour"*. Verified
  otherwise: `luaL_newmetatable` returns the *existing* table and
  `register_class` mutates it in place, so re-registering a class updates
  objects that already exist.

  ```
  before reload, obj:ver() =        1
  after re-register, OLD obj:ver() = 2
  ```

  The rest of the paragraph (captured tables, registered callbacks) is right.
  Fix the sentence, and add a test pinning the real behaviour — it is a
  *feature*, and an undocumented one.
- **`luakit_embed_dir` is undocumented in the README**, which describes only
  `luakit_embed_script`. `src/` uses `_dir` exclusively, and `_dir` is the one
  with the interesting `STUB_DIR` story.
- **The `T *` push/get asymmetry is unstated.** Returning `nullptr` pushes nil,
  but a `T *` parameter rejects nil (`Userdata<T>::check` raises). Correct, and
  `std::optional<T *>` is the answer, but a reader has to discover that.
- **No `CONTRIBUTING`/`CHANGELOG`**, and `luakit` has no license file at all —
  worth fixing before anyone is invited to vendor `lib/`.

---

## Suggested order

**Phase 1 — correctness, ~1 week.** 1.2 (`invalidate`), 1.3 and 1.5 (both
small), 1.1 (derived-type resolution, the substantial one), Part 5 doc fixes.
Land CI first so the rest is protected.

**Phase 2 — type coverage, ~3 days.** `const T *`, `const T &`,
`unique_ptr<T>`, `Stack<Ref>`, sets. All additive, all test-per-item.

**Phase 3 — the control block, ~1 week.** 3.2. Do this before 3.1, which wants
somewhere to keep its counters. Decide 1.4 (`Coroutine` tuple semantics) here
too, since it is the last breaking change on the list.

**Phase 4 — the features. ~2 weeks.** 3.4 (GC) and 3.5 (the small ones) first —
they are independent and immediately useful. Then 3.1 (sandboxing), which is
the flagship.

**Phase 5 — LuaLS definitions, ~1 week.** 3.3. Deliberately last: it wants the
registration surface to have stopped moving, and it is the one that most
changes how the library *feels* to the people it is for.

Infrastructure items from Part 4 fold in alongside — header self-containment
and compile-fail tests with Phase 2, benchmarks with Phase 1's `dynamic_cast`
path.

---

## Implementation notes

What changed from the plan as written, and what was learned doing it.

**1.6 (lax strings) was documented, not changed.** As proposed: matching
`luaL_checkstring` is defensible and tightening it would break working scripts.

**A latent bug turned up in `invoke_and_push`.** The lambda wrapping the bound
call had a deduced return type, and `auto` decays `T &` to `T` — so the moment
1.5 allowed a method to return a reference to a *different* object, the binding
tried to copy it. Invisible before only because every `T &` return in the tree
was the chaining case, which never reaches that path. Both call sites now spell
the return type out.

**The instruction budget does reach script-created coroutines.** This was an
open question when the sandbox was designed — `lua_sethook` is per-thread, so a
`coroutine.wrap` made inside Lua could have run unbounded. Lua 5.4 copies the
parent's hook into new threads, so it does not. Pinned by a test rather than
left to a reading of the source.

**Most-derived-type resolution refuses one case on purpose.** If the derived
type is registered but never declared its bases, resolving to it would produce
a value that a parameter of the *base* type could not read back — trading one
broken direction for another. `Userdata<D>::reaches` checks first and the push
falls back to the static type. `test_polymorphic.cpp` covers it.

**RTTI is detected, not required.** `-fno-rtti` is a normal choice in game
builds and disables `typeid`/`dynamic_cast`. Without it a base pointer stays
the base type — exactly the old behaviour — and CI builds that configuration.

### What the benchmark found

Running `bench/bench.cpp` turned up something reading had not. The README's
"a lookup costs the same however deep the hierarchy is" holds for a class's
*own* members — depth 0 and depth 4 are within noise of each other — but
reaching an *inherited* member costs roughly twice as much:

```
own method, no bases                         107.5 ns/op
own method, four deep                        104.8 ns/op
inherited method, one level up               248.5 ns/op
inherited method, four levels up             315.5 ns/op
own field, four deep                         120.9 ns/op
inherited field, four levels up              148.5 ns/op
```

Depth is genuinely free — flattening at registration is doing its job. What
costs is converting the *receiver*: a method declared on `Animal`, called on a
`Dog`, misses the exact-metatable check in `Userdata<Animal>::try_get` and goes
through the cast table, which is three extra Lua table operations. Constant in
depth, but not free.

This is pre-existing and was not introduced by any change here. It is left
alone: fixing it would mean caching the resolved pointer per (userdata, type)
pair, which is real complexity for a cost that is still tens of nanoseconds
against a call that already costs a hundred. The claim in the README is now the
measured one, and the benchmark will catch it if that stops being true.
