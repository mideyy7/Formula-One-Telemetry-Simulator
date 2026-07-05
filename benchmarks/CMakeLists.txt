add_executable(rcz_bench bench_main.cpp)

target_link_libraries(rcz_bench PRIVATE rcz_lib)

# Release-mode optimisations are set per-target so they apply even when
# the root build type is Debug (e.g. running tests in Debug + bench in Release).
target_compile_options(rcz_bench PRIVATE -O3 -DNDEBUG)