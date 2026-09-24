# One binary per test file, so a crash in one suite cannot hide the others.

file(GLOB LUAKIT_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(test_src IN LISTS LUAKIT_TEST_SOURCES)
  get_filename_component(test_name "${test_src}" NAME_WE)

  add_executable(${test_name} "${test_src}")
  target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${test_name} PRIVATE luakit::luakit luakit::warnings)

  # Several cases assert "nothing leaked" or "no double free", which only a
  # sanitizer can observe. Without this they still pass, but prove much less.
  if(LUNA_SANITIZE)
    target_compile_options(${test_name} PRIVATE -fsanitize=address,undefined
                                                -fno-omit-frame-pointer -g)
    target_link_options(${test_name} PRIVATE -fsanitize=address,undefined)
  endif()

  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT
                                               "ASAN_OPTIONS=detect_leaks=1")
endforeach()

# ------------------------------------------------------- refusing to compile
#
# Some of what this library promises is enforced at compile time and cannot be
# observed at run time at all: that a borrowed view may not escape the call
# that produced it, that a Variadic must come last, that an unregistered class
# is named as such. A suite of things that *do* work says nothing about any of
# it -- the assertions could all be deleted and every test here would pass.
#
# So each one gets a target that must fail to build, and must fail saying the
# sentence it was written to say.
function(luakit_add_compile_fail name expect)
  add_library(cf_${name} OBJECT EXCLUDE_FROM_ALL
              "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/${name}.cpp")
  target_link_libraries(cf_${name} PRIVATE luakit::luakit)

  add_test(
    NAME compile_fail_${name}
    COMMAND
      "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DTARGET=cf_${name}"
      "-DEXPECT=${expect}" -P
      "${CMAKE_SOURCE_DIR}/cmake/expect-compile-failure.cmake")
endfunction()

luakit_add_compile_fail(variadic_not_last "a Variadic parameter must come last")
luakit_add_compile_fail(borrowed_table_get "Table::get cannot return a borrowed type")
luakit_add_compile_fail(borrowed_coroutine_resume "Coroutine::resume cannot return a borrowed type")
luakit_add_compile_fail(unique_ptr_parameter "would take ownership away from Lua")
luakit_add_compile_fail(method_on_data_member "method<> takes a pointer to a member function")
luakit_add_compile_fail(missing_metatable "needs a Metatable<T> declaring")
luakit_add_compile_fail(overaligned_userdata "over-aligned for Lua userdata storage")
