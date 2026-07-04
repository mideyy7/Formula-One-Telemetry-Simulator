cmake_minimum_required(VERSION 3.20)
project(RaceCondition-z CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)

# FIXUI - terminal UI Library
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

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