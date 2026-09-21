# luakit

Header-only C++20 bindings for Lua 5.4, for adding a plugin or modding layer to
a program that already exists.

The premise is that the C++ side is the real program. luakit is not a framework
to build around; it is what you reach for when a game or tool needs scripts to
call into it, be called back from it, and work with the objects it already owns.

```cpp
// Plain C++. Nothing here knows about Lua.
auto spawn(std::string name) -> Entity * { return world.emplace(std::move(name)); }

auto luaopen_world(luakit::core::State *L) -> int {
  return luakit::Class<Entity>(L)
      .method<&Entity::damage>("damage")
      .prop<&Entity::hp>("hp")
      .ro_prop<&Entity::name>("name")
      .meta<&Entity::describe>("__tostring")
      .fn<spawn>("spawn")
      .build_module();
}
```

```lua
local world = require("world")

local goblin = world.spawn("goblin")
goblin:damage(30)
print(goblin.hp, goblin.name)   --> 70   goblin
```

## Layout

```
lib/luakit/        the library, header-only
lib/cmake/         FindLua54 and luakit_embed_script
src/               luna, a demo host that embeds it
test/              one binary per suite, run under ASan and UBSan
tools/gen_api.py   regenerates the mechanical half of the core:: layer
```

`lib/` is self-contained and can be copied into another project, pulled in with
`FetchContent`, or installed and found with `find_package(luakit)`.

Each header stands on its own, so you include what you name:

| Header | What it gives you |
| --- | --- |
| `interpreter.hpp` | `Interpreter`, and through it `Table` and `Error` |
| `function.hpp` | `fn`, `method`, `ctor` |
| `class.hpp` | the `Class<T>` builder, and everything below it |
| `property.hpp` | `prop`, `ro_prop`, `accessor` |
| `overload.hpp` | `fn_overload`, `method_overload`, `ctor_overload`, `Args` |
| `callback.hpp` | `Function`, for calling Lua from C++ |
| `table.hpp` | `Table` |
| `coroutine.hpp` | `Coroutine`, `yielding` |
| `enums.hpp` | `EnumNames`, `EnumEntry` |
| `variadic.hpp` | `Variadic` |
| `userdata.hpp` | `Metatable`, `Bases`, `Ownership`, `Userdata<T>` |
| `stack.hpp` | `Stack<T>`, the conversion contract |
| `ref.hpp` | `Ref`, a registry anchor for any Lua value |
| `guard.hpp` | `guard`, the exception boundary |
| `core/api.hpp` | the whole Lua C API, namespaced |

## Using it

```cmake
add_subdirectory(luakit)
target_link_libraries(my_game PRIVATE luakit::luakit)
```

```cpp
luakit::Interpreter lua;
lua.open_libs()
   .preload({"world", luaopen_world})   // available to require, not yet run
   .script_file("mods/init.lua");
```

`Interpreter` owns the `lua_State` and closes it. Failures -- a chunk that will
not load, an error at runtime -- arrive as `luakit::Error`, with a traceback
attached, rather than as a status code to forget to check.

## Exposing C++ to Lua

### Functions

`luakit::fn` reads the C++ signature and does the rest: argument count, type
checking, conversion, and return values.

```cpp
auto scale(std::vector<double> v, double by) -> std::vector<double>;

const luakit::core::aux::Reg funcs[] = {
    {"scale", luakit::fn<scale>},
    {nullptr, nullptr},
};
```

A wrong argument is reported the way Lua reports its own:
`bad argument #1 to 'scale' (element 2 is not a number)`.

### Classes

A class needs a name Lua knows it by:

```cpp
template <> struct luakit::Metatable<Entity> {
  static constexpr const char *k_name = "world.Entity";
};
```

and then either the `Class<T>` builder shown above, or the `Reg` arrays it is
sugar for -- `luakit::method<&T::f>`, `luakit::ctor<T, Args...>`,
`luakit::prop<&T::field>("name")`.

A method returning `T &` is the chaining idiom: `e:damage(1):damage(2)`.

### Fields

`prop` exposes a data member, `ro_prop` makes it read-only, and `accessor`
backs a field with getter and setter methods so it can be computed or
validated. Assignments are type-checked exactly as arguments are.

Any name that is not a method or a property becomes per-instance state on the
object, so a mod can stash its own data on someone else's entity:

```lua
goblin.my_mod_state = { aggro = true }   -- kept with the object, collected with it
```

### Inheritance

```cpp
template <> struct luakit::Metatable<Dog> {
  static constexpr const char *k_name = "world.Dog";
  using bases = luakit::Bases<Animal>;
};
```

A `Dog` then satisfies a parameter of type `Animal *` or `Animal &`, with the
pointer adjusted properly for a base at a non-zero offset. Methods, fields and
metamethods are all copied down at registration, so a base's `__tostring` is
what a derived class prints with, and a lookup costs the same however deep the
hierarchy is. A name the derived class defines itself wins.

`__gc`, `__index` and `__newindex` are the exception: luakit owns all three and
sets them last. A base's `__gc` on a derived class would check for the wrong
metatable and never destroy anything, and the index pair is the field dispatch.

Bases must be registered before the classes that inherit from them.

### Overloads

One Lua name, several signatures, tried in the order written:

```cpp
.overload<&Vec::scaled_by_number, &Vec::scaled_by_vec>("scaled")
.ctors<luakit::Args<>, luakit::Args<double, double>>("new")
```

### Enums and variadics

An enum reaches Lua as a string, so a misspelling is caught at the call with a
message listing the alternatives:

```cpp
template <> struct luakit::EnumNames<Facing> {
  static constexpr luakit::EnumEntry<Facing> k_values[] = {
      {"north", Facing::north}, {"south", Facing::south},
  };
};
```

`luakit::Variadic`, as the last parameter, collects whatever else was passed.

## Calling Lua from C++

`luakit::Function` is a Lua function held by C++. It is an ordinary parameter
type, so a plugin registers a handler the same way it calls anything else:

```cpp
auto on_tick(luakit::Function cb) -> void { handlers.push_back(std::move(cb)); }

// ... later, from the engine loop
for (const auto &h : handlers) h.call<void>(dt);
```

Calls are protected: an error inside a plugin arrives as a `luakit::Error` with
a traceback, not as a crashed host.

One rule comes with it. A `Function` holds a reference into the registry and
gives it back when it is destroyed, so it must not outlive the `Interpreter`.
A handler container that is a namespace-scope static gets destroyed after
`main` returns, which is after `lua_close` -- release them first.
`src/packages/world/world.cpp` does this in `world::shutdown`, driven by a guard in
`main` so an early return cannot skip it.

`luakit::Table` does the same for tables, which is usually how a plugin's
configuration arrives:

```cpp
auto cfg = lua.global<luakit::Table>("config");
const int width = cfg.get_or<int>("width", 1280);   // default if absent or wrong
const auto title = cfg.get<std::string>("title");   // throws if absent or wrong
```

## Object ownership

The question a binding layer has to answer is who destroys what. luakit answers
it per object rather than per class:

| Mode | Created by | Destroyed by | Used for |
| --- | --- | --- | --- |
| owned | `ctor`, or returning `T` by value | Lua's collector | objects a script makes for itself |
| borrowed | returning `T *`, or `push_ref` | nobody | the engine's own entities, lent out |
| shared | returning `std::shared_ptr<T>` | the last reference | objects neither side should have to outlive |

All three share one metatable, so a method works the same whichever it has.

Pushing the same pointer twice gives the same Lua object, so `a == b` holds and
a field a script set on it is still there next time. If the host has to destroy
something a script can still reach, `Userdata<T>::invalidate` severs it: the
next use says `used after finalization` instead of reading freed memory.

## Coroutines

`luakit::Coroutine` drives a Lua coroutine from C++ with typed resumes. Going
the other way, `luakit::yielding` binds a C++ function that suspends its caller,
which is what a mod needs to write `sleep(2)`:

```cpp
auto sleep(double seconds) -> double { scheduler.wake_in(seconds); return seconds; }
{"sleep", luakit::yielding<sleep>}
```

Whatever the host passes to the next resume becomes the call's result.

## Hot reload

`lua.reload("plugin")` drops a module and requires it again, so a script can be
edited without restarting. It does not retrofit the new code onto anything the
old version left behind -- captured tables, registered callbacks, objects built
from the previous metatable all keep the old behaviour -- so it works best for
plugins whose state lives in the module table.

## Embedding scripts

`luakit_embed_script` bakes a script into the executable, as bytecode or as
source:

```cmake
luakit_embed_script(TARGET my_game SCRIPT lua/init.lua NAME init_lua MODE bytecode STRIP)
```

It defines `init_lua[]`, `init_lua_len` and the macro `LUAKIT_EMBEDDED_INIT_LUA`,
for `Interpreter::script_bytecode`.

## How it stays safe

Three invariants do most of the work, and are worth knowing if you write
anything by hand against the `core::` layer.

**A C++ exception must never unwind through Lua's C frames.** Everything
`fn`, `method` and `ctor` produce is already wrapped in `luakit::guard`, which
turns an exception into a Lua error. A hand-written `core::CFunction` that can
throw needs the same wrapper.

**A Lua error is a longjmp and skips destructors.** So `Stack<T>` splits its
work into phases that are kept apart: `test` never raises, `check` may raise but
builds nothing, `get` may build things but never raises. A bound call runs every
`check` before any `get`, which is why a bad argument three cannot strand an
object built for argument one.

**A value taken from the stack may be a view into it.** `Stack<T>::borrows`
says which conversions those are, and the places that would outlive the stack
slot -- `Coroutine::resume`, `Function::call`, `Table::get` -- refuse them at
compile time.

## Building

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
cmake --build build --target run      # the demo host
```

Tests build with AddressSanitizer and UBSan by default: several of them assert
"nothing leaked" or "nothing was freed twice", which only a sanitizer can see.

`cmake --build build --target api` regenerates `lib/luakit/core/api_gen.hpp`,
which is only needed when the Lua version changes. `format` and `format-check`
run clang-format and stylua.
