#pragma once

// A placeholder for the real file_scripts.hpp: luakit_embed_dir
// generates that one into the build directory once CMake has configured the
// project, and it is always found ahead of this one -- STUB_DIR is added
// with -idirafter, which GCC/Clang only fall back to once every ordinary -I
// path has come up empty. So this file exists purely so an editor's LSP has
// something to resolve luakit::file::script() against before that
// first configure; the placeholder entry below never matches a real path,
// so find_script() throws for every one, which is fine, since nothing here
// is meant to ever actually run.
//
// Configured by luakit_embed_dir from lib/luakit/embedded_stub.hpp.in --
// do not hand-edit the copy it writes into STUB_DIR.

#include "luakit/script.hpp"

#include <string_view>

namespace luakit {
namespace file {

// A truly empty array breaks find_script<N>'s template deduction, so this
// carries one entry whose path ("") no real script path can ever equal.
inline constexpr NamedScript table[] = {
    {"", {"", nullptr, 0}},
};

inline auto script(std::string_view path) -> Script { return find_script(table, path); }

}  // namespace file
}  // namespace luakit
