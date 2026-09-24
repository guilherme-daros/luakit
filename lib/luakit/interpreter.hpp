#pragma once

#include "luakit/core/api.hpp"
#include "luakit/error.hpp"
#include "luakit/guard.hpp"
#include "luakit/module.hpp"
#include "luakit/ref.hpp"
#include "luakit/script.hpp"
#include "luakit/stack.hpp"
#include "luakit/state.hpp"
#include "luakit/table.hpp"
#include "luakit/userdata.hpp"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace luakit {

namespace detail {

// The sink an on_print handler is held in. A std::function rather than a plain
// function pointer, because the thing a host wants to route print into is
// almost always a method on something -- a console, a log, a network sink.
using PrintSink = std::function<void(std::string_view)>;

inline auto sink_gc(core::State *L) noexcept -> int {
  std::destroy_at(static_cast<PrintSink *>(core::touserdata(L, 1)));
  return 0;
}

// print, rebuilt to hand the line to the sink instead of stdout. Follows the
// real one exactly: tab between arguments, __tostring honoured, no trailing
// newline of its own -- the sink decides what a line ends with.
inline auto print_impl(core::State *L) -> int {
  auto *sink = static_cast<PrintSink *>(core::touserdata(L, core::upvalueindex(1)));

  std::string line;
  const int n = core::gettop(L);
  for (int i = 1; i <= n; ++i) {
    std::size_t len = 0;
    core::aux::tolstring(L, i, &len);
    const char *s = core::tolstring(L, -1, &len);
    if (i > 1) line += '\t';
    line.append(s, len);
    core::pop(L, 1);
  }

  (*sink)(line);
  return 0;
}

}  // namespace detail

// The standard libraries, for opening one at a time with Interpreter::openlib
// rather than all of them with Interpreter::openlibs. Named and ordered the
// way luaL_openlibs' own internal table is.
enum class Lib { base, package, coroutine, table, io, os, string, math, utf8, debug };

// Collection, driven by the host rather than by Lua's own schedule.
//
// A game loop wants to say "do some collecting, but not for long" at a point
// of its choosing, which is what step() is for; a loading screen wants
// collect(). Lua 5.4's generational mode is often the better fit for a host
// that allocates steadily every frame, and is one call away here.
class Gc {
 public:
  explicit Gc(core::State *L) noexcept : L_(L) {}

  // A full collection cycle. The blunt one, for a moment that is already slow.
  auto collect() -> Gc & {
    core::gc(L_, core::GCCOLLECT);
    return *this;
  }

  // One incremental step. Returns true when that finished a cycle.
  auto step(int kbytes = 0) -> bool { return core::gc(L_, core::GCSTEP, kbytes) != 0; }

  auto stop() -> Gc & {
    core::gc(L_, core::GCSTOP);
    return *this;
  }
  auto restart() -> Gc & {
    core::gc(L_, core::GCRESTART);
    return *this;
  }
  auto running() const -> bool { return core::gc(L_, core::GCISRUNNING) != 0; }

  // Bytes Lua currently has allocated. Counted the way Lua counts, in
  // kilobytes plus a remainder, and put back together here.
  auto bytes() const -> std::size_t {
    const auto kb = static_cast<std::size_t>(core::gc(L_, core::GCCOUNT));
    const auto rem = static_cast<std::size_t>(core::gc(L_, core::GCCOUNTB));
    return kb * 1024 + rem;
  }

  auto incremental(int pause, int stepmul, int stepsize) -> Gc & {
    core::gc(L_, core::GCINC, pause, stepmul, stepsize);
    return *this;
  }

  auto generational(int minormul, int majormul) -> Gc & {
    core::gc(L_, core::GCGEN, minormul, majormul);
    return *this;
  }

 private:
  core::State *L_;
};

// Owns a lua_State. This is the host-side entry point; code running inside a
// bound callback is handed a borrowed core::State* instead and must not own it,
// which is why Stack<T> and the fn/method wrappers all take the raw pointer.
class Interpreter {
 public:
  Interpreter() : Interpreter(Limits{}) {}

  // An interpreter with resource limits, for running code the host did not
  // write. See Limits in state.hpp for what each one bounds and why.
  explicit Interpreter(const Limits &limits) : handle_(std::make_shared<StateHandle>()) {
    handle_->memory = limits.memory;
    handle_->instructions = limits.instructions;

    // lua_newstate rather than luaL_newstate, because the allocator is the
    // only place a memory cap can be enforced, and it has to be in place
    // before the state exists. The panic handler is what luaL_newstate would
    // have installed.
    L_ = core::newstate(&detail::limited_alloc, handle_.get());
    if (!L_) throw Error("luakit: cannot create lua_State");

    core::atpanic(L_, &detail::panic);
    detail::install_handle(L_, handle_.get());
  }

  ~Interpreter() { close(); }

  Interpreter(Interpreter &&other) noexcept : L_(std::exchange(other.L_, nullptr)), handle_(std::move(other.handle_)) {}

  auto operator=(Interpreter &&other) noexcept -> Interpreter & {
    if (this != &other) {
      close();
      L_ = std::exchange(other.L_, nullptr);
      handle_ = std::move(other.handle_);
    }
    return *this;
  }

  Interpreter(const Interpreter &) = delete;
  auto operator=(const Interpreter &) -> Interpreter & = delete;

  // Closes the state early, rather than waiting for the destructor.
  //
  // Anything still holding a registry reference -- a Function a plugin
  // registered, a Table the host read a config out of -- keeps working as an
  // object afterwards and says so when used, instead of touching a lua_State
  // that is gone. See state.hpp for how.
  auto close() noexcept -> void {
    if (!L_) return;
    if (handle_) handle_->open = false;  // must precede lua_close
    core::close(L_);
    L_ = nullptr;
  }

  // Escape hatch, for mixing with hand-written C API code.
  auto raw() const noexcept -> core::State * { return L_; }

  // Collection control, for a host that would rather decide when.
  auto gc() const noexcept -> Gc { return Gc(L_); }

  // Bytes currently allocated, as this interpreter's own allocator counts
  // them. Cheaper than gc().bytes(), and the number a memory Limit is
  // measured against.
  auto memory_used() const noexcept -> std::size_t { return handle_ ? handle_->used : 0; }

  auto openlibs() -> Interpreter & {
    core::aux::openlibs(L_);
    return *this;
  }

  // Opens a single standard library, the way luaL_openlibs would open just
  // that one: registered in package.loaded and as a global under its usual
  // name (e.g. Lib::string gives you both package.loaded.string and _G.string).
  auto openlib(Lib lib) -> Interpreter & {
    switch (lib) {
      case Lib::base:
        core::aux::requiref(L_, core::GNAME, core::lib::base, 1);
        break;
      case Lib::package:
        core::aux::requiref(L_, core::LOADLIBNAME, core::lib::package, 1);
        break;
      case Lib::coroutine:
        core::aux::requiref(L_, core::COLIBNAME, core::lib::coroutine, 1);
        break;
      case Lib::table:
        core::aux::requiref(L_, core::TABLIBNAME, core::lib::table, 1);
        break;
      case Lib::io:
        core::aux::requiref(L_, core::IOLIBNAME, core::lib::io, 1);
        break;
      case Lib::os:
        core::aux::requiref(L_, core::OSLIBNAME, core::lib::os, 1);
        break;
      case Lib::string:
        core::aux::requiref(L_, core::STRLIBNAME, core::lib::string, 1);
        break;
      case Lib::math:
        core::aux::requiref(L_, core::MATHLIBNAME, core::lib::math, 1);
        break;
      case Lib::utf8:
        core::aux::requiref(L_, core::UTF8LIBNAME, core::lib::utf8, 1);
        break;
      case Lib::debug:
        core::aux::requiref(L_, core::DBLIBNAME, core::lib::debug, 1);
        break;
    }
    core::pop(L_, 1);  // requiref leaves the module table on the stack
    return *this;
  }

  // Advertise a module to require without loading it. Must follow openlibs
  // (or at least openlib(Lib::package)), which is what creates `package`.
  auto preload(Module lib) -> Interpreter & {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::PRELOAD_TABLE);
    core::pushcfunction(L_, lib.open);
    core::setfield(L_, -2, lib.name);
    core::pop(L_, 1);
    return *this;
  }

  template <typename T>
  auto bind(const core::aux::Reg *methods, const core::aux::Reg *meta = nullptr, const PropertyReg *props = nullptr)
      -> Interpreter & {
    Userdata<T>::register_class(L_, methods, meta, props);
    return *this;
  }

  auto script(std::string_view code, const char *chunkname = "=chunk") -> void { eval<void>(code, chunkname); }

  // Accepts precompiled bytecode or source; luaL_loadbuffer sniffs the
  // "\x1bLua" signature. The chunk name is only a label for tracebacks.
  auto script_bytecode(const void *data, std::size_t len, const char *chunkname) -> void {
    finish<void>(core::aux::loadbuffer(L_, static_cast<const char *>(data), len, chunkname));
  }

  auto script_file(const char *path) -> void { finish<void>(core::aux::loadfile(L_, path)); }

  // Runs a chunk and reads back what it returns.
  //
  //   lua.eval<int>("return 2 + 3")             // 5
  //   lua.eval<Table>(settings_source)          // a config file's table
  //   lua.eval<std::tuple<int, int>>("...")     // two results, as a call
  //
  // script() is this with R = void, and stays, because discarding the result
  // is the common case. But a config file whose last line is `return { ... }`
  // is the other common case, and reading it used to mean hand-written stack
  // code around load_script_as_module.
  template <typename R = void>
  auto eval(std::string_view code, const char *chunkname = "=eval") -> R {
    return finish<R>(core::aux::loadbuffer(L_, code.data(), code.size(), chunkname));
  }

  template <typename R = void>
  auto eval_file(const char *path) -> R {
    return finish<R>(core::aux::loadfile(L_, path));
  }

  // Runs a Script from luakit_embed_dir's generated table, whichever of
  // embedded bytes or a disk path it turned out to hold.
  template <typename R = void>
  auto load(const Script &s) -> R {
    if (s.data) {
      return finish<R>(core::aux::loadbuffer(L_, reinterpret_cast<const char *>(s.data), s.len, s.chunkname));
    }
    return finish<R>(core::aux::loadfile(L_, s.path));
  }

  // ------------------------------------------------------------ sandboxing
  //
  // Which libraries are open says what a plugin may reach for; Limits says how
  // much it may use. This is the third lever: what a particular chunk can see
  // of the globals, whatever else is open.
  //
  //   auto env = lua.make_env({"print", "pairs", "ipairs", "string", "math"});
  //   lua.script_in(env, untrusted_source, "=mods/thing.lua");
  //
  // Names are taken from _G as they stand, so "string" brings that library
  // whole. For anything finer, build the table with Table::set instead -- this
  // returns an ordinary Table and the host is free to keep editing it.
  //
  // A name that is not in _G is skipped rather than refused: asking for a
  // library the host never opened should narrow the sandbox, not fail.
  auto make_env(std::initializer_list<const char *> allow) -> Table {
    Table env = Table::create(L_, 0, static_cast<int>(allow.size()) + 1);
    Table g = globals();

    for (const char *name : allow) {
      if (g.has(name)) env.set(name, g.get<Ref>(name));
    }

    // Chunks reach their own environment through _G, and load() inside the
    // sandbox needs it to exist, so it points at the sandbox rather than at
    // the real globals -- which would hand back everything just withheld.
    env.push(L_);
    env.set("_G", Ref::pop(L_));
    return env;
  }

  // Runs a chunk with env as its globals. _ENV is upvalue 1 of every main
  // chunk, which is the whole mechanism.
  auto script_in(const Table &env, std::string_view code, const char *chunkname = "=chunk") -> void {
    fail_if(core::aux::loadbuffer(L_, code.data(), code.size(), chunkname), "cannot load");

    env.push(L_);
    if (!core::setupvalue(L_, -2, 1)) {  // pops the table on success
      core::pop(L_, 2);
      throw Error("luakit: chunk has no _ENV to replace");
    }

    const int base = core::gettop(L_) - 1;
    detail::StackRestore restore(L_, base);
    detail::arm_budget(L_);
    if (detail::call_traced(L_, 0, 0) != core::OK) throw detail::traced_error(L_, "error running");
  }

  // ------------------------------------------------------------- plumbing

  // Where require looks for .lua files. A host loading mods out of a directory
  // needs this, and had to reach past the facade into the package table.
  auto package_path(std::string_view pattern) -> Interpreter & { return set_package_field("path", pattern); }
  auto package_cpath(std::string_view pattern) -> Interpreter & { return set_package_field("cpath", pattern); }

  // Puts a pattern ahead of what is already there, so the host's own mod
  // directory is searched before the system ones.
  auto add_package_path(std::string_view pattern) -> Interpreter & {
    const auto current = globals().get<Table>("package").get_or<std::string>("path", "");
    return package_path(current.empty() ? std::string(pattern) : std::string(pattern) + ";" + current);
  }

  // Sends everything scripts print to the host instead of to stdout, which is
  // what a game with its own console wants. The sink is called once per print
  // with the whole line, tabs between arguments and no trailing newline.
  auto on_print(detail::PrintSink sink) -> Interpreter & {
    void *mem = core::newuserdatauv(L_, sizeof(detail::PrintSink), 0);
    new (mem) detail::PrintSink(std::move(sink));

    // The sink holds captured state that has to be destroyed with the state,
    // so it is a userdata with a finalizer rather than a light one.
    core::createtable(L_, 0, 1);
    core::pushcfunction(L_, &detail::sink_gc);
    core::setfield(L_, -2, "__gc");
    core::setmetatable(L_, -2);

    core::pushcclosure(L_, &guard<&detail::print_impl>, 1);
    core::setglobal(L_, "print");
    return *this;
  }

  // Forgets a loaded module, so the next require runs its opener or file
  // again. Unknown module names are not an error: the point is to end up with
  // it not loaded.
  auto unload(const char *module) -> Interpreter & {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::LOADED_TABLE);
    core::pushnil(L_);
    core::setfield(L_, -2, module);
    core::pop(L_, 1);
    return *this;
  }

  // Reloads a module in place, which is what makes editing a plugin without
  // restarting the host possible.
  //
  // What this does not do is retrofit the new code onto anything the old
  // version left behind. A table a script already captured, a callback the
  // host is still holding, an object built from the previous metatable: all of
  // them keep the old behaviour. Reloading works for plugins whose state lives
  // in the module table and nowhere else, which is worth designing for if hot
  // reload matters.
  //
  // A module that fails to load is left unloaded rather than half-loaded, so
  // fixing the script and reloading again is the way out.
  auto reload(const char *module) -> Interpreter & {
    unload(module);

    core::getglobal(L_, "require");
    if (!core::isfunction(L_, -1)) {
      core::pop(L_, 1);
      throw Error("luakit: reload needs the standard libraries; call openlibs first");
    }
    core::pushstring(L_, module);
    detail::arm_budget(L_);
    if (detail::call_traced(L_, 1, 0) != core::OK) throw detail::traced_error(L_, "error reloading");
    return *this;
  }

  // Whether a module is currently loaded.
  auto loaded(const char *module) -> bool {
    core::aux::getsubtable(L_, core::REGISTRYINDEX, core::LOADED_TABLE);
    const bool present = core::getfield(L_, -1, module) != core::TNIL;
    core::pop(L_, 2);
    return present;
  }

  // The globals table, _G.
  auto globals() -> Table { return Table::globals(L_); }

  // Reads a global, throwing if it is absent or not a T. The common case by
  // far, so it is spelled out rather than left to globals().get<T>().
  template <typename T>
  auto global(const char *name) -> T {
    return globals().template get<T>(name);
  }

  // Reads a global, falling back when it is absent or the wrong type.
  template <typename T>
  auto global_or(const char *name, T fallback) -> T {
    return globals().template get_or<T>(name, std::move(fallback));
  }

  template <typename T>
  auto set_global(const char *name, T &&value) -> Interpreter & {
    globals().set(name, std::forward<T>(value));
    return *this;
  }

 private:
  // Loads then calls, converting either failure into an Error and reading back
  // however many results R asks for. The call is traced: a script author whose
  // plugin fails ten frames deep gets the frames rather than just the
  // innermost line, and Error::traceback keeps them reachable on their own.
  template <typename R>
  auto finish(int load_status) -> R {
    static_assert(!detail::result_borrows<R>,
                  "luakit: a chunk's result cannot be a borrowed type. The values leave the stack "
                  "before this returns, so a std::string_view or const char * would outlive them. "
                  "Use std::string.");

    fail_if(load_status, "cannot load");

    const int base = core::gettop(L_) - 1;  // below the chunk just loaded
    detail::StackRestore restore(L_, base);

    detail::arm_budget(L_);
    if (detail::call_traced(L_, 0, detail::result_count<R>) != core::OK) {
      throw detail::traced_error(L_, "error running");
    }
    return detail::read_results<R>(L_, base);
  }

  auto fail_if(int status, const char *what) -> void {
    if (status == core::OK) return;
    throw Error(std::string(what) + ": " + detail::take_error(L_));
  }

  auto set_package_field(const char *field, std::string_view value) -> Interpreter & {
    globals().get<Table>("package").set(field, std::string(value));
    return *this;
  }

  core::State *L_ = nullptr;
  std::shared_ptr<StateHandle> handle_;
};

}  // namespace luakit
