# Benchmarks are off by default (LUNA_BUILD_BENCH) and never sanitized: ASan
# changes the numbers by more than any of the differences being looked for.
#
# google/benchmark does the statistical work a hand-rolled timer loop cannot:
# it calibrates how many repetitions a measurement needs, and
# --benchmark_repetitions plus --benchmark_report_aggregates_only=false turns
# that into a mean, median and stddev instead of one sample. Fetched rather
# than vendored, and only when LUNA_BUILD_BENCH is on, so it never touches the
# default build or lib/'s zero-dependency story.

include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_LIBPFM OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE luakit::luakit benchmark::benchmark)
