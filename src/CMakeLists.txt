# Phase 2: STATIC now that we have a .cpp source file (thread_pool.cpp).
add_library(rcz_lib STATIC
    concurrency/thread_pool.cpp
    simulation/telemetry_generator.cpp
    race_control/track_limits.cpp
    race_control/penalty_enforcer.cpp
    race_control/weather.cpp
    strategy/strategy_analyzer.cpp
)

target_include_directories(rcz_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rcz_lib PUBLIC
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Minimal executable — verifies the project compiles.
# main.cpp will be populated in Step 3.
add_executable(RaceCondition-z main.cpp)
target_link_libraries(RaceCondition-z PRIVATE rcz_lib)