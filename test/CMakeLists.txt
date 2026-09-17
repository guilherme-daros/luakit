# One binary per test file, so a crash in one suite cannot hide the others.

file(GLOB LUAKIT_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(test_src IN LISTS LUAKIT_TEST_SOURCES)
  get_filename_component(test_name "${test_src}" NAME_WE)

  add_executable(${test_name} "${test_src}")
  target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${test_name} PRIVATE luakit::luakit)
  target_compile_options(${test_name} PRIVATE -Wall -Wextra -pedantic)

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
