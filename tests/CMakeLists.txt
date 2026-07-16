enable_testing()

# Helper: creates a test executable for one test suite.
# Usage: add_test_suite(test_foo file1.cpp file2.cpp ...)
function(add_test_suite suite_name)
    add_executable(${suite_name} ${ARGN})
    target_link_libraries(${suite_name}
        PRIVATE
            rcz_lib
            GTest::gtest
            GTest::gtest_main
    )
    target_include_directories(${suite_name}
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME ${suite_name} COMMAND ${suite_name})
endfunction()

add_test_suite(test_types test_types/test_types.cpp)

add_test_suite(test_concurrency
    test_concurrency/test_spsc_queue.cpp
    test_concurrency/test_mpsc_queue.cpp
    test_concurrency/test_thread_pool.cpp
)

add_test_suite(test_telemetry
    test_telemetry/test_telemetry_generator.cpp
)

add_test_suite(test_simulation
    test_simulation/test_lap_time_consumer.cpp
)

add_test_suite(test_race_control
    test_race_control/test_track_limits.cpp
    test_race_control/test_penalty_enforcer.cpp
    test_race_control/test_weather.cpp
)

add_test_suite(test_shared_state
    test_shared_state/test_leaderboard.cpp
    test_shared_state/test_race_state.cpp
)
