# Benchmarks are off by default and never sanitized: ASan changes the numbers
# by more than any of the differences being looked for.

add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE luakit::luakit luakit::warnings)
