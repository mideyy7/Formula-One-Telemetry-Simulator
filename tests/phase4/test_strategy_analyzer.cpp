#include <gtest/gtest.h>
#include "strategy/strategy_analyzer.h"
#include "common/season_data.h"
#include <future>
#include <vector>

TEST(StrategyAnalyzerTest, ReturnsValidPitLap) {
    ThreadPool pool{4};
    StrategyAnalyzer analyzer{pool};

    DriverState driver;
    driver.profile = DRIVERS[0]; // VER
    driver.car     = CARS[0];    // Red Bull

    auto result = analyzer.analyze(driver, DEFAULT_TRACK, 50);

    bool valid = false;
    for (int c : StrategyAnalyzer::CANDIDATES) {
        if (c == result.pit_lap) { valid = true; break; }
    }
    EXPECT_TRUE(valid) << "pit_lap=" << result.pit_lap;
    EXPECT_GT(result.estimated_race_time_s, 0.0f);
}

TEST(StrategyAnalyzerTest, AllFuturesComplete) {
    ThreadPool pool{4};
    StrategyAnalyzer analyzer{pool};

    DriverState driver;
    driver.profile = DRIVERS[1];
    driver.car     = CARS[0];

    // If any future deadlocks, this test will hang.
    // Run with a timeout in your CI (e.g. --timeout 10).
    auto result = analyzer.analyze(driver, DEFAULT_TRACK, 50);
    EXPECT_GT(result.estimated_race_time_s, 0.0f);
}

TEST(StrategyAnalyzerTest, ConcurrentAnalysesNoDeadlock) {
    ThreadPool pool{8};
    StrategyAnalyzer analyzer{pool};

    // Run 5 analyses simultaneously using std::async.
    std::vector<std::future<StrategyResult>> results;
    for (int i = 0; i < 5; ++i) {
        DriverState d;
        d.profile = DRIVERS[i];
        d.car     = CARS[i / 2];
        results.push_back(std::async(std::launch::async,
            [&analyzer, d] {
                return analyzer.analyze(d, DEFAULT_TRACK, 50);
            }));
    }
    for (auto& f : results) {
        auto r = f.get();
        EXPECT_GT(r.estimated_race_time_s, 0.0f);
    }
}