// Namespaced view of the Lua 5.4 C API.
//
//   luakit::core::*       core API      (lua_*)
//   luakit::core::aux::*  auxiliary lib (luaL_*)
//   luakit::core::lib::*  standard libs (luaopen_*)
//
// Functions, types and constants are mechanical aliases and live in the
// generated api_gen.hpp. What follows are the parts that cannot be aliased:
// Lua ships 62 of its API entry points as preprocessor macros, which have no
// address and no namespace. Each one is rewritten here as a real function.
//
// Deliberately omitted: the nine LUA_COMPAT_APIINTCASTS shims (luaL_checkint,
// lua_tounsigned, ...) and the internal output macros (lua_writestring,
// lua_writeline, lua_writestringerror, luaL_intop).

#pragma once

#include "luakit/core/api_gen.hpp"

#include <cstddef>

namespace luakit::core {

// ---- stack ----
inline auto pop(State *L, int n) -> void {
  lua_pop(L, n);
}
inline auto insert(State *L, int idx) -> void {
  lua_insert(L, idx);
}
inline auto remove(State *L, int idx) -> void {
  lua_remove(L, idx);
}
inline auto replace(State *L, int idx) -> void {
  lua_replace(L, idx);
}
inline constexpr auto upvalueindex(int i) -> int {
  return lua_upvalueindex(i);
}

// ---- type predicates ----
inline auto isboolean(State *L, int n) -> bool {
  return lua_isboolean(L, n);
}
inline auto isfunction(State *L, int n) -> bool {
  return lua_isfunction(L, n);
}
inline auto islightuserdata(State *L, int n) -> bool {
  return lua_islightuserdata(L, n);
}
inline auto isnil(State *L, int n) -> bool {
  return lua_isnil(L, n);
}
inline auto isnone(State *L, int n) -> bool {
  return lua_isnone(L, n);
}
inline auto isnoneornil(State *L, int n) -> bool {
  return lua_isnoneornil(L, n);
}
inline auto istable(State *L, int n) -> bool {
  return lua_istable(L, n);
}
inline auto isthread(State *L, int n) -> bool {
  return lua_isthread(L, n);
}

// ---- conversions (the NULL-defaulted forms) ----
inline auto tonumber(State *L, int i) -> Number {
  return lua_tonumber(L, i);
}
inline auto tointeger(State *L, int i) -> Integer {
  return lua_tointeger(L, i);
}
inline auto tostring(State *L, int i) -> const char * {
  return lua_tostring(L, i);
}

// ---- push / call ----
inline auto newtable(State *L) -> void {
  lua_newtable(L);
}
inline auto pushcfunction(State *L, CFunction f) -> void {
  lua_pushcfunction(L, f);
}
inline auto pushglobaltable(State *L) -> void {
  lua_pushglobaltable(L);
}
inline auto pushliteral(State *L, const char *s) -> const char * {
  return lua_pushstring(L, s);
}
inline auto register_(State *L, const char *n, CFunction f) -> void {
  lua_register(L, n, f);
}
inline auto call(State *L, int nargs, int nresults) -> void {
  lua_call(L, nargs, nresults);
}
inline auto pcall(State *L, int nargs, int nresults, int errfunc) -> int {
  return lua_pcall(L, nargs, nresults, errfunc);
}
inline auto yield(State *L, int nresults) -> int {
  return lua_yield(L, nresults);
}

// ---- userdata ----
inline auto newuserdata(State *L, std::size_t sz) -> void * {
  return lua_newuserdata(L, sz);
}
inline auto getuservalue(State *L, int idx) -> int {
  return lua_getuservalue(L, idx);
}
inline auto setuservalue(State *L, int idx) -> int {
  return lua_setuservalue(L, idx);
}
inline auto getextraspace(State *L) -> void * {
  return lua_getextraspace(L);
}

namespace aux {

inline auto checkversion(State *L) -> void {
  luaL_checkversion(L);
}
inline auto checkstring(State *L, int n) -> const char * {
  return luaL_checkstring(L, n);
}
inline auto optstring(State *L, int n, const char *d) -> const char * {
  return luaL_optstring(L, n, d);
}
inline auto typename_(State *L, int i) -> const char * {
  return luaL_typename(L, i);
}
inline auto getmetatable(State *L, const char *n) -> int {
  return luaL_getmetatable(L, n);
}
inline auto pushfail(State *L) -> void {
  luaL_pushfail(L);
}

inline auto loadbuffer(State *L, const char *s, std::size_t sz, const char *name) -> int {
  return luaL_loadbuffer(L, s, sz, name);
}
inline auto loadfile(State *L, const char *f) -> int {
  return luaL_loadfile(L, f);
}
inline auto dofile(State *L, const char *f) -> int {
  return luaL_dofile(L, f);
}
inline auto dostring(State *L, const char *s) -> int {
  return luaL_dostring(L, s);
}

// luaL_newlib cannot be forwarded: it sizes the table with
// sizeof(l)/sizeof(l[0]), which is meaningless once `l` decays to a pointer.
inline auto newlib(State *L, const Reg *l) -> void {
  lua_newtable(L);
  luaL_setfuncs(L, l, 0);
}
inline auto newlibtable(State *L, int n) -> void {
  lua_createtable(L, 0, n);
}

// ---- buffers ----
inline auto addchar(Buffer *B, char c) -> void {
  luaL_addchar(B, c);
}
inline auto addsize(Buffer *B, std::size_t s) -> void {
  luaL_addsize(B, s);
}
inline auto buffsub(Buffer *B, int s) -> void {
  luaL_buffsub(B, s);
}
inline auto buffaddr(Buffer *B) -> char * {
  return luaL_buffaddr(B);
}
inline auto bufflen(Buffer *B) -> std::size_t {
  return luaL_bufflen(B);
}
inline auto prepbuffer(Buffer *B) -> char * {
  return luaL_prepbuffer(B);
}

// luaL_opt takes a checker function as an argument, so it becomes a template
// rather than a plain wrapper: lua::aux::opt(L, aux::checkinteger, 2, 0).
template <typename F, typename D>
inline auto opt(State *L, F check, int n, D d) -> D {
  return lua_isnoneornil(L, n) ? d : static_cast<D>(check(L, n));
}

// ---- argument checking ----
inline auto argcheck(State *L, bool cond, int arg, const char *extramsg) -> void {
  luaL_argcheck(L, cond, arg, extramsg);
}
inline auto argexpected(State *L, bool cond, int arg, const char *tname) -> void {
  luaL_argexpected(L, cond, arg, tname);
}

}  // namespace aux

}  // namespace luakit::core
