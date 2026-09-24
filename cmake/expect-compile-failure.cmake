# Drives one compile-fail test: build a target that must not compile, and
# check that it failed for the stated reason.
#
# The reason matters as much as the failure. Several of luakit's best
# properties are static_asserts whose whole value is the sentence they print --
# "use prop<> instead", "a Variadic must come last" -- and a test that only
# asserted "this does not build" would keep passing after the message had
# decayed into a template backtrace.
#
# Run as:
#   cmake -DBUILD_DIR=... -DTARGET=... -DEXPECT=... -P expect-compile-failure.cmake

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout_text
  ERROR_VARIABLE stderr_text)

set(output "${stdout_text}${stderr_text}")

if(status EQUAL 0)
  message(FATAL_ERROR "${TARGET}: expected a compile error, but it built cleanly")
endif()

string(FIND "${output}" "${EXPECT}" found_at)
if(found_at EQUAL -1)
  message(
    FATAL_ERROR
      "${TARGET}: refused to compile, but not for the documented reason.\n"
      "Expected to find: ${EXPECT}\n"
      "Got:\n${output}")
endif()

message(STATUS "${TARGET}: refused, saying \"${EXPECT}\"")
