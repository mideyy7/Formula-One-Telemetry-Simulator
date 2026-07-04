enable_testing()

# Helper: creates a test executable for one phase.
# Usage: add_phase_tests(phase1 file1.cpp file2.cpp ...)
function(add_phase_tests phase_name)
    add_executable(${phase_name}_tests ${ARGN})
    target_link_libraries(${phase_name}_tests
        PRIVATE
            rcz_lib
            GTest::gtest
            GTest::gtest_main
    )
    target_include_directories(${phase_name}_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME ${phase_name} COMMAND ${phase_name}_tests)
endfunction()

add_phase_tests(phase1 phase1/test_types.cpp)

add_phase_tests(phase2
    phase2/test_spsc_queue.cpp
    phase2/test_mpsc_queue.cpp
    phase2/test_thread_pool.cpp
)

# Uncomment each block as you complete the phase:
# add_phase_tests(phase3
#     phase3/test_telemetry_generator.cpp
#     phase3/test_latch.cpp
#     phase3/test_barrier.cpp
#     phase3/test_semaphore.cpp
# )
# add_phase_tests(phase4
#     phase4/test_track_limits.cpp
#     phase4/test_penalty_enforcer.cpp
#     phase4/test_weather.cpp
#     phase4/test_strategy_analyzer.cpp
# )
# add_phase_tests(phase5
#     phase5/test_leaderboard.cpp
#     phase5/test_race_state.cpp
# )
# add_phase_tests(phase6 phase6/test_dashboard_integration.cpp)