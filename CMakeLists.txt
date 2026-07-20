cmake_minimum_required(VERSION 3.20)
project(RaceCondition-z CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# Phase 3 uses std::jthread / std::stop_token. Apple's libc++ ships these
# behind an "experimental" flag (they exist, just disabled by default).
# GCC/libstdc++ needs no such flag, so only add it for Clang.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fexperimental-library)
endif()

include(FetchContent)

# GoogleTest
FetchContent_Declare(googleTest
    GIT_REPOSITORY https://github.com/google/googletest
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # specific for windows
# - on Windows, mixing static/dynamic C runtimes between the code and gtest can cause link errors. This forces gtest to use the shared runtime to match typical project settings. 
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(benchmarks)