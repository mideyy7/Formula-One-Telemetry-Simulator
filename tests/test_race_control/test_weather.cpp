#include <gtest/gtest.h>
#include "race_control/weather.h"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

TEST(WeatherTest, InitiallyDry) {
    MpscQueue<RaceControlEvent> q;
    WeatherSystem wx{q};
    EXPECT_EQ(wx.current(), WeatherState::DRY);
    EXPECT_FLOAT_EQ(wx.grip_factor(), 1.0f);
}

TEST(WeatherTest, ValidTransitionsOnly) {
    MpscQueue<RaceControlEvent> q;
    WeatherSystem wx{q};

    // Force many updates — state should only ever be DRY, DAMP, or WET.
    for (int lap = 0; lap < 200; lap += 5) {
        wx.update(lap);
        WeatherState s = wx.current();
        EXPECT_TRUE(s == WeatherState::DRY ||
                    s == WeatherState::DAMP ||
                    s == WeatherState::WET);
    }
}

// SWMR test: 8 reader threads + 1 writer thread.
// Readers must never see a torn/invalid WeatherState.
TEST(WeatherTest, SharedMutexNoTearing) {
    MpscQueue<RaceControlEvent> q;
    WeatherSystem wx{q};
    std::atomic<bool> stop{false};
    std::atomic<bool> violation{false};

    // 8 reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                WeatherState s = wx.current();
                if (s != WeatherState::DRY &&
                    s != WeatherState::DAMP &&
                    s != WeatherState::WET) {
                    violation.store(true);
                }
            }
        });
    }

    // 1 writer thread — updates every 5ms
    std::thread writer([&] {
        for (int lap = 0; lap < 200; lap += 5) {
            wx.update(lap);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stop.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_FALSE(violation.load());
}