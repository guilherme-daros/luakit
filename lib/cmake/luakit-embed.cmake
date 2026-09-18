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
