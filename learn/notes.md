# Day 1
- Claude made the plan for the project spliiting it into phases
- Learn more about CMake and especially this line `set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) `
- Inspect CMakeLists.txt in src, tets and root directory
- cmake -B build makes the build directory and cmake --build build runs it
- read std::array on learncpp.com
- know the use of each attribute in the struct of each of the types
- NOT YET GONE IN DEPTH INTO WHAT PHASE 1 TESTS MEAN - automated with AI tests


# dAY 2
Two real bugs I caught by test-compiling before writing the real files (both explained in the lesson doc):

std::jthread/std::stop_token need -fexperimental-library on Apple Clang — added conditionally.
std::barrier<std::function<void() noexcept>> doesn't compile on this libc++ — worked around with a noexcept lambda wrapper instead.
A third bug (template capacity mismatch in a test) got caught by the compiler during the build itself.

learn/lesson3.md now has a full file-by-file, function-by-function walkthrough, the gotchas, a "what I'd defend vs. what I'd fix" design retrospective, and a quiz section for interview prep.