#include <gtest/gtest.h>
#include "common/leaderboard.h"
#include "common/season_data.h"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

static std::vector<DriverState> make_standings(int n = 20) {
    std::vector<DriverState> v;
    for (int i = 0; i < n; ++i) {
        DriverState s;
        s.profile  = DRIVERS[i];
        s.car      = CARS[i / 2];
        s.position = i + 1;
        s.latest_frame.driver_id = DRIVERS[i].id;
        v.push_back(s);
    }
    return v;
}

TEST(LeaderboardTest, UpdateAndSnapshot) {
    Leaderboard board;
    auto standings = make_standings();
    board.update(standings);

    auto snap = board.snapshot();
    ASSERT_EQ(snap.size(), 20u);
    EXPECT_EQ(snap[0].profile.id, "VER");
}

TEST(LeaderboardTest, SnapshotIsFullCopy) {
    Leaderboard board;
    board.update(make_standings());

    auto snap = board.snapshot();
    EXPECT_EQ(snap.size(), 20u);
    // Modifying the snapshot should not affect the leaderboard.
    snap[0].position = 99;
    EXPECT_EQ(board.at_position(1).profile.id, "VER");
}

// SWMR stress: 1 writer updates every 10ms; 8 readers snapshot as fast as
// possible for 2 seconds. Readers must always see all 20 drivers.
TEST(LeaderboardTest, SwmrNoTearing) {
    Leaderboard board;
    board.update(make_standings());

    std::atomic<bool> stop{false};
    std::atomic<bool> violation{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = board.snapshot();
                if (snap.size() != 20u) violation.store(true);
            }
        });
    }

    std::thread writer([&] {
        for (int i = 0; i < 200; ++i) {
            board.update(make_standings());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_FALSE(violation.load()) << "Saw partial standings under concurrent read";
}

TEST(LeaderboardTest, AtPosition) {
    Leaderboard board;
    board.update(make_standings());

    auto p1 = board.at_position(1);
    EXPECT_EQ(p1.position, 1);
    EXPECT_EQ(p1.profile.id, "VER");

    auto invalid = board.at_position(99);
    EXPECT_EQ(invalid.position, 0); // default DriverState
}