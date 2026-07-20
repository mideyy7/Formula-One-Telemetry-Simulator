#include <gtest/gtest.h>
#include "common/leaderboard.h"
#include "common/season_data.h"
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <stdexcept>

static std::vector<DriverState> make_standings(int n = 20) {
    std::vector<DriverState> v;
    for (int i = 0; i < n; ++i) {
        DriverState s;
        s.profile  = DRIVERS[i];
        s.car      = CARS[i / 2];
        s.position = i + 1;
        std::memcpy(s.latest_frame.driver_id, DRIVERS[i].id.data(), 3);
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
//
// The initial seed update happens on the writer thread itself (not the test's
// main thread) -- update() now enforces single-writer, so every call must
// come from the same thread for this board's lifetime. Readers wait for
// `ready` before starting so they don't observe the board before it's seeded.
TEST(LeaderboardTest, SwmrNoTearing) {
    Leaderboard board;

    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> violation{false};

    std::thread writer([&] {
        board.update(make_standings());
        ready.store(true, std::memory_order_release);
        for (int i = 1; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            board.update(make_standings());
        }
        stop.store(true);
    });

    while (!ready.load(std::memory_order_acquire)) {} // wait for the seed update

    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = board.snapshot();
                if (snap.size() != 20u) violation.store(true);
            }
        });
    }

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_FALSE(violation.load()) << "Saw partial standings under concurrent read";
}

// Single-writer is enforced, not just documented: a second thread calling
// update() must be rejected rather than silently accepted.
TEST(LeaderboardTest, SecondWriterThreadThrows) {
    Leaderboard board;
    board.update(make_standings()); // claims the writer slot for this thread

    std::thread other([&] {
        EXPECT_THROW(board.update(make_standings()), std::logic_error);
    });
    other.join();

    // The original (claiming) thread can still update freely.
    EXPECT_NO_THROW(board.update(make_standings()));
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