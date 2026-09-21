# luakit_embed_script -- bake a Lua script into an executable as a byte array.
#
# Every host that embeds Lua ends up writing this: compile the script, turn the
# bytes into a C array, and hand them to Interpreter::script_bytecode. It is
# plumbing rather than policy, so it lives with the library.
#
#   luakit_embed_script(
#     TARGET  my_game
#     SCRIPT  "${CMAKE_SOURCE_DIR}/lua/init.lua"
#     NAME    init_lua          # optional; defaults to the file stem
#     MODE    bytecode          # bytecode (default) or source
#     STRIP                     # bytecode only: drop line numbers and locals
#   )
#
# Defines, on the target, an include directory holding "<name>.h" which
# declares:
#
#   unsigned char <name>[];
#   unsigned int  <name>_len;
#
# and the macro LUAKIT_EMBEDDED_<NAME>, so a source file can tell whether the
# script was baked in or is being read from disk.
#
# bytecode is faster to start but is tied to the exact Lua build that produced
# it, so luac must be the one shipped alongside the liblua being linked. source
# is immune to that skew at the cost of parsing at startup.

function(luakit_embed_script)
  set(options STRIP)
  set(one_value TARGET SCRIPT NAME MODE)
  cmake_parse_arguments(LES "${options}" "${one_value}" "" ${ARGN})

  if(NOT LES_TARGET)
    message(FATAL_ERROR "luakit_embed_script: TARGET is required")
  endif()
  if(NOT LES_SCRIPT)
    message(FATAL_ERROR "luakit_embed_script: SCRIPT is required")
  endif()
  if(NOT EXISTS "${LES_SCRIPT}")
    message(FATAL_ERROR "luakit_embed_script: no such script '${LES_SCRIPT}'")
  endif()

  if(NOT LES_MODE)
    set(LES_MODE bytecode)
  endif()
  if(NOT LES_MODE MATCHES "^(bytecode|source)$")
    message(FATAL_ERROR
            "luakit_embed_script: MODE must be bytecode or source (got '${LES_MODE}')")
  endif()

  if(NOT LES_NAME)
    get_filename_component(stem "${LES_SCRIPT}" NAME_WE)
    string(MAKE_C_IDENTIFIER "${stem}_lua" LES_NAME)
  endif()

  # Per target and per name, so two scripts in one target cannot collide.
  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/luakit-embed/${LES_TARGET}/${LES_NAME}")
  set(header "${gen_dir}/${LES_NAME}.h")

  if(LES_MODE STREQUAL "bytecode")
    find_program(LUAKIT_LUAC_EXECUTABLE NAMES luac5.4 luac REQUIRED)

    set(luac_flags "")
    if(LES_STRIP)
      set(luac_flags -s)
    endif()

    set(compiled "${gen_dir}/${LES_NAME}.luac")
    add_custom_command(
      OUTPUT "${compiled}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
      COMMAND "${LUAKIT_LUAC_EXECUTABLE}" ${luac_flags} -o "${compiled}" "${LES_SCRIPT}"
      DEPENDS "${LES_SCRIPT}"
      COMMENT "luakit: compiling ${LES_NAME} to bytecode"
      VERBATIM)
    set(input "${compiled}")
  else()
    set(input "${LES_SCRIPT}")
  endif()

  # xxd is only looked for when something is actually being embedded, so a
  # build that loads from disk does not require it to be installed.
  find_program(LUAKIT_XXD_EXECUTABLE NAMES xxd REQUIRED)

  add_custom_command(
    OUTPUT "${header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
    COMMAND "${LUAKIT_XXD_EXECUTABLE}" -i -n "${LES_NAME}" "${input}" "${header}"
    DEPENDS "${input}"
    COMMENT "luakit: embedding ${LES_NAME} (${LES_MODE})"
    VERBATIM)

  # Listing the generated header as a source is what ties it to the target and
  # gets the commands above run.
  target_sources(${LES_TARGET} PRIVATE "${header}")
  target_include_directories(${LES_TARGET} PRIVATE "${gen_dir}")

  string(TOUPPER "${LES_NAME}" upper_name)
  target_compile_definitions(${LES_TARGET} PRIVATE "LUAKIT_EMBEDDED_${upper_name}")
endfunction()

# luakit_embed_dir -- the same idea, for every .lua script in a directory,
# behind one generated lookup table instead of one macro per script.
#
#   luakit_embed_dir(
#     TARGET      my_game
#     SOURCE_DIR  "${CMAKE_SOURCE_DIR}/lua"
#     NAMESPACE   file            # generates `namespace file { ... }`
#     MODE        bytecode        # bytecode, source or file
#     STRIP                       # bytecode only: drop line numbers and locals
#     STUB_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/lsp_stub"  # optional
#   )
#
# Generates, on the target, an include directory holding
# "<namespace>_scripts.hpp", which declares:
#
#   namespace luakit {
#   namespace <namespace> {
#     auto script(std::string_view path) -> Script;
#   }
#   }
#
# so a caller reaches it as luakit::<namespace>::script(path) -- nested under
# luakit, since this is luakit's own naming convention for it, not a name the
# consumer picked. `path` is a script's path relative to CMAKE_SOURCE_DIR,
# e.g. "lua/init.lua" for "${CMAKE_SOURCE_DIR}/lua/init.lua". In
# bytecode/source mode this calls luakit_embed_script per file and the
# returned Script is backed by the embedded bytes; in file mode it is backed
# by that file's absolute path instead, read at runtime. Either way the
# shape is chosen once, by CMake, at configure time -- no macro appears in
# the generated header or in whatever calls script().
#
# STUB_DIR, if given, also configures lib/luakit/embedded_stub.hpp.in (a real
# file that ships with luakit, not text built up inline here) into a checked-
# in placeholder "<namespace>_scripts.hpp" under that directory -- one entry
# whose path can never match a real one, so find_script() throws for every
# path -- and adds that directory to the target with -idirafter, which
# GCC/Clang only fall back to once every ordinary -I path has come up empty.
# The real generated header therefore always wins once it exists; the
# placeholder is only ever seen by an editor's LSP before the project has
# been configured, when the real one does not exist yet. Its content never
# depends on which scripts exist, only on NAMESPACE, so it is safe to commit
# and forget.
function(luakit_embed_dir)
  set(options STRIP)
  set(one_value TARGET SOURCE_DIR NAMESPACE MODE STUB_DIR)
  cmake_parse_arguments(LED "${options}" "${one_value}" "" ${ARGN})

  if(NOT LED_TARGET)
    message(FATAL_ERROR "luakit_embed_dir: TARGET is required")
  endif()
  if(NOT LED_SOURCE_DIR)
    message(FATAL_ERROR "luakit_embed_dir: SOURCE_DIR is required")
  endif()
  if(NOT LED_NAMESPACE)
    message(FATAL_ERROR "luakit_embed_dir: NAMESPACE is required")
  endif()
  if(NOT LED_MODE MATCHES "^(bytecode|source|file)$")
    message(FATAL_ERROR
            "luakit_embed_dir: MODE must be bytecode, source or file (got '${LED_MODE}')")
  endif()

  file(GLOB scripts CONFIGURE_DEPENDS "${LED_SOURCE_DIR}/*.lua")
  list(SORT scripts)

  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/luakit-embed/${LED_TARGET}/${LED_NAMESPACE}")
  set(header "${gen_dir}/${LED_NAMESPACE}_scripts.hpp")

  set(includes "")
  set(entries "")

  foreach(script IN LISTS scripts)
    file(RELATIVE_PATH key "${CMAKE_SOURCE_DIR}" "${script}")
    get_filename_component(stem "${script}" NAME_WE)
    string(MAKE_C_IDENTIFIER "${stem}_lua" name)

    if(LED_MODE STREQUAL "file")
      string(APPEND entries "    {\"${key}\", {\"@${key}\", nullptr, 0, \"${script}\"}},\n")
    else()
      set(strip_arg "")
      if(LED_STRIP)
        set(strip_arg STRIP)
      endif()
      luakit_embed_script(
        TARGET "${LED_TARGET}"
        SCRIPT "${script}"
        NAME "${name}"
        MODE "${LED_MODE}"
        ${strip_arg})
      string(APPEND includes "#include \"${name}.h\"\n")
      string(APPEND entries "    {\"${key}\", {\"@${key}\", ${name}, ${name}_len}},\n")
    endif()
  endforeach()

  set(content "#pragma once\n\n#include \"luakit/script.hpp\"\n\n")
  string(APPEND content "${includes}\n")
  string(APPEND content "namespace luakit {\nnamespace ${LED_NAMESPACE} {\n\n")
  # xxd's generated arrays are plain (non-const) globals, so this table can't
  # be constexpr when it references them -- const is enough either way, since
  # find_script only ever reads it at runtime.
  string(APPEND content "inline const NamedScript table[] = {\n")
  string(APPEND content "${entries}")
  string(APPEND content "};\n\n")
  string(APPEND content
         "inline auto script(std::string_view path) -> Script { return find_script(table, path); }\n\n")
  string(APPEND content "}  // namespace ${LED_NAMESPACE}\n}  // namespace luakit\n")

  file(WRITE "${header}" "${content}")

  target_sources(${LED_TARGET} PRIVATE "${header}")
  target_include_directories(${LED_TARGET} PRIVATE "${gen_dir}")

  if(LED_STUB_DIR)
    # The stub's actual content lives in lib/luakit/embedded_stub.hpp.in, a
    # real file that ships with luakit -- @NAMESPACE@ is its only variable.
    # In-tree, that is a sibling of lib/cmake/, where this file lives.
    # Installed, headers move under <prefix>/include while this file stays
    # under <prefix>/lib/cmake/luakit, so lib/CMakeLists.txt installs a copy
    # of the template alongside it instead.
    set(stub_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../luakit/embedded_stub.hpp.in")
    if(NOT EXISTS "${stub_template}")
      set(stub_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embedded_stub.hpp.in")
    endif()

    set(NAMESPACE "${LED_NAMESPACE}")
    configure_file("${stub_template}" "${LED_STUB_DIR}/${LED_NAMESPACE}_scripts.hpp" @ONLY)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${LED_TARGET} PRIVATE "-idirafter${LED_STUB_DIR}")
    endif()
  endif()
endfunction()
