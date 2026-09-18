#!/usr/bin/env python3
"""Generate lib/luakit/core/api_gen.hpp from the Lua 5.4 public headers.

Only the mechanical parts are generated: exported functions, typedefs and
constants. Function-like macros cannot be aliased and are hand-written in
api.hpp.

Usage: python3 tools/gen_api.py [--lua-include DIR] [-o OUT]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

# Names that are C++ keywords, or otherwise unusable as identifiers, get a
# trailing underscore.
CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "class", "compl", "concept", "const",
    "consteval", "constexpr", "constinit", "continue", "decltype", "default",
    "delete", "do", "double", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "not", "nullptr", "operator",
    "or", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "template", "this", "throw", "true", "try", "typedef", "typeid",
    "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "while", "xor",
}

FUNC_RE = re.compile(
    r"^(?:LUA_API|LUALIB_API|LUAMOD_API)\b.*?\((lua|luaL|luaopen)_(\w+)\)\s*\("
)
TYPEDEF_RE = re.compile(r"^typedef\b.*?\b(lua|luaL)_(\w+)\s*;")
STRUCT_TYPEDEF_RE = re.compile(r"^\}\s*(lua|luaL)_(\w+)\s*;")
# typedef int (*lua_CFunction) (lua_State *L);
FNPTR_TYPEDEF_RE = re.compile(r"^typedef\b.*?\(\s*\*\s*(lua|luaL)_(\w+)\s*\)\s*\(")
# The \s+ after the name is what excludes function-like macros (those have the
# paren tight against the name). Values may themselves start with '(' -- e.g.
# LUA_MULTRET (-1) -- so no lookahead here.
DEFINE_CONST_RE = re.compile(r"^#define\s+(LUA[A-Z_]*_\w+)\s+\S")


def safe(name: str) -> str:
    return name + "_" if name in CPP_KEYWORDS else name


def parse(path: pathlib.Path):
    funcs, types, consts = [], [], []
    for line in path.read_text().splitlines():
        m = FUNC_RE.match(line)
        if m:
            funcs.append((m.group(1), m.group(2)))
            continue
        m = (FNPTR_TYPEDEF_RE.match(line) or TYPEDEF_RE.match(line)
             or STRUCT_TYPEDEF_RE.match(line))
        if m:
            types.append((m.group(1), m.group(2)))
            continue
        m = DEFINE_CONST_RE.match(line)
        if m and not m.group(1).endswith("_API"):
            consts.append(m.group(1))
    return funcs, types, consts


def compiles(body: str, inc: str) -> tuple[bool, set[int]]:
    """Compile a candidate TU; return (ok, set of failing 1-based line numbers)."""
    src = f'#include <lua.hpp>\nnamespace probe {{\n{body}\n}}\n'
    with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as f:
        f.write(src)
        tmp = f.name
    proc = subprocess.run(
        ["g++", "-std=c++17", "-fsyntax-only", f"-I{inc}", tmp],
        capture_output=True, text=True,
    )
    pathlib.Path(tmp).unlink()
    if proc.returncode == 0:
        return True, set()
    bad = set()
    for m in re.finditer(rf"{re.escape(tmp)}:(\d+):", proc.stderr):
        # body starts at line 3 of the generated TU
        bad.add(int(m.group(1)) - 2)
    return False, bad


def const_alias(name: str) -> str:
    """LUA_OK -> OK, LUAL_BUFFERSIZE -> BUFFERSIZE, LUAI_MAXSTACK -> MAXSTACK."""
    for prefix in ("LUAL_", "LUAI_", "LUA_"):
        if name.startswith(prefix):
            return safe(name[len(prefix):])
    return safe(name)


def filter_constants(consts: list[str], inc: str) -> list[tuple[str, str]]:
    """Keep only constants usable as `inline constexpr auto` values.

    Several LUA_* defines are type macros (LUA_NUMBER -> double) or contain
    sizeof expressions, so the only reliable test is compilation. The probe
    emits the *real* alias names, because a name that collides with its own
    macro (LUAL_NUMSIZES -> LUAL_NUMSIZES) only breaks in that form.
    """
    keep = [(const_alias(c), c) for c in consts]
    for _ in range(60):
        body = "\n".join(f"inline constexpr auto {a} = {c};" for a, c in keep)
        ok, bad = compiles(body, inc)
        if ok:
            return keep
        if not bad:
            break
        keep = [p for i, p in enumerate(keep) if (i + 1) not in bad]
    return keep


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lua-include", default="/usr/include/lua5.4")
    ap.add_argument("-o", "--out", default="lib/luakit/core/api_gen.hpp")
    args = ap.parse_args()

    inc = pathlib.Path(args.lua_include)
    core_f, core_t, core_c = parse(inc / "lua.h")
    aux_f, aux_t, aux_c = parse(inc / "lauxlib.h")
    lib_f, _, lib_c = parse(inc / "lualib.h")

    consts = filter_constants(sorted(set(core_c + aux_c + lib_c)), str(inc))

    out = [
        "// GENERATED by tools/gen_api.py -- do not edit.",
        "// Mechanical aliases for the Lua 5.4 C API. Macros live in api.hpp.",
        "//",
        "// api.hpp is the only supported way in. Every alias below is declared",
        "// here, so without the pragma include-cleaner sees this file as the",
        "// provider and tells callers to include it directly.",
        "",
        '// IWYU pragma: private, include "luakit/core/api.hpp"',
        "",
        "#pragma once",
        "",
        "#include <lua.hpp>",
        "",
        "static_assert(LUA_VERSION_NUM == 504, \"luakit targets Lua 5.4\");",
        "",
        "namespace luakit::core {",
        "",
    ]

    def emit_types(ts, prefix):
        for p, n in sorted(set(ts)):
            out.append(f"using {safe(n)} = ::{p}_{n};")

    def emit_funcs(fs, prefix):
        for p, n in sorted(set(fs)):
            out.append(f"inline constexpr auto {safe(n)} = &::{p}_{n};")

    out.append("// ---- types ----")
    emit_types(core_t, "lua")
    out.append("")
    out.append("// ---- constants ----")
    seen: dict[str, str] = {}
    for alias, c in consts:
        if alias in seen:
            print(f"warning: alias collision {alias}: {seen[alias]} and {c}", file=sys.stderr)
            continue
        seen[alias] = c
        out.append(f"inline constexpr auto {alias} = {c};")
    out.append("")
    out.append("// ---- core API (lua_*) ----")
    emit_funcs([f for f in core_f if f[0] == "lua"], "lua")

    # Namespace by prefix, not by declaring header: luaL_* is always aux::,
    # luaopen_* is always lib::. That rule is predictable without knowing
    # which header happens to declare a given name (luaL_openlibs lives in
    # lualib.h but is still aux::).
    out += ["", "namespace aux {", "", "// ---- types ----"]
    emit_types(aux_t, "luaL")
    out.append("")
    out.append("// ---- auxiliary library (luaL_*) ----")
    emit_funcs([f for f in aux_f + lib_f if f[0] == "luaL"], "luaL")
    out += ["", "}  // namespace aux", ""]

    out += ["namespace lib {", "", "// ---- standard library openers (luaopen_*) ----"]
    for p, n in sorted(set(f for f in core_f + aux_f + lib_f if f[0] == "luaopen")):
        out.append(f"inline constexpr auto {safe(n)} = &::luaopen_{n};")
    out += ["", "}  // namespace lib", "", "}  // namespace luakit::core", ""]

    dest = pathlib.Path(args.out)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text("\n".join(out))

    print(f"wrote {dest}")
    print(f"  types      {len(set(core_t)) + len(set(aux_t))}")
    print(f"  constants  {len(consts)} (of {len(set(core_c + aux_c + lib_c))} candidates)")
    print(f"  core fns   {len(set(f for f in core_f if f[0] == 'lua'))}")
    print(f"  aux fns    {len(set(f for f in aux_f if f[0] == 'luaL'))}")
    print(f"  lib fns    {len(set(lib_f))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
