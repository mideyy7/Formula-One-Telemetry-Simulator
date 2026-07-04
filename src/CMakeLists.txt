# Phase 1: header-only library (no .cpp files yet).
# Phase 3 will upgrade this to STATIC when .cpp source files are added.
add_library(rcz_lib INTERFACE)

target_include_directories(rcz_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rcz_lib INTERFACE
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Minimal executable — verifies the project compiles.
# main.cpp will be populated in Step 3.
add_executable(RaceCondition-z main.cpp)
target_link_libraries(RaceCondition-z PRIVATE rcz_lib)